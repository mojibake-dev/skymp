#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>

PartOne& GetPartOne();

// thuum docs/verbs/traps.md (ADR-059 decision 4): the cell's host reports a
// trap's hit on another player's body; the server checks the number against
// the trap's own table and the hit against the trap's reach, then applies
// the table's value. The stage is ObjectAnimationTest's temple 0x371de with
// the swinging wall TrapWallWood 0x54b15 (base 0xc6440, whose
// TrapSwingingWall table is 75, 100, 125, 150, 175, 200 in Skyrim.esm's
// VMAD) at (8428.4, -5421.5, -1922.1); the portcullis 0x3b9f1 is the
// scripted world object without a table.
namespace {
constexpr uint32_t kHostActor = 0xff000fe5;
constexpr uint32_t kGuestActor = 0xff000fe6;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kWall = 0x54b15;
constexpr uint32_t kPortcullis = 0x3b9f1;
// a game-file NPC the server loads under npcEnabled (Alvor 0x13482 in
// Riverwood); the target check precedes the cell check
constexpr uint32_t kAlvor = 0x13482;
// a hundred units off the wall
const NiPoint3 kByTheWall{ 8428.f, -5321.f, -1922.f };
// past wire-rules traps::REACH from it
const NiPoint3 kFarFromTheWall{ 8428.f, -4721.f, -1922.f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// `user` hosts `cell` (TrapRelayTest's shape)
void Host(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage loaded;
  loaded.cell = cell;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  p.TickCellHosts(past);
}

void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kByTheWall, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kByTheWall, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  p.worldState.GetFormAt<MpObjectReference>(kWall);
  p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(kByTheWall);
    actor.ForceSubscriptionsUpdate();
  }
  Host(p, 0, kTemple);
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

void Hit(PartOne& p, Networking::UserId user, uint32_t refr, uint32_t target,
         float damage)
{
  TrapHitMessage msg;
  msg.refr = refr;
  msg.target = target;
  msg.damage = damage;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnTrapHit(raw, msg);
}

float Health(PartOne& p, uint32_t actor)
{
  return p.worldState.GetFormAt<MpActor>(actor)
    .GetChangeForm()
    .actorValues.healthPercentage;
}

size_t ChangeValuesTo(PartOne& p, Networking::UserId user)
{
  size_t n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == user &&
        dynamic_cast<ChangeValuesMessage*>(m.message.get())) {
      ++n;
    }
  }
  return n;
}
}

TEST_CASE("The host's trap hit on a guest's body applies the trap's table "
          "value to the guest's record, and a second one kills it with no "
          "killer",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  auto& guest = p.worldState.GetFormAt<MpActor>(kGuestActor);
  REQUIRE(Health(p, kGuestActor) == 1.f);
  const float base = guest.GetMaximumValues().health;
  REQUIRE(base > 75.f);

  Hit(p, 0, kWall, kGuestActor, 75.f);
  REQUIRE(Health(p, kGuestActor) == Catch::Approx(1.f - 75.f / base));
  REQUIRE(ChangeValuesTo(p, 1) >= 1);

  // a number the wall's scripts cannot pick changes nothing
  p.Messages().clear();
  Hit(p, 0, kWall, kGuestActor, 76.f);
  REQUIRE(Health(p, kGuestActor) == Catch::Approx(1.f - 75.f / base));
  REQUIRE(ChangeValuesTo(p, 1) == 0);

  // the table's largest value (a float the engine would deal as 200) ends it:
  // the server's own kill, credited to nobody
  Hit(p, 0, kWall, kGuestActor, 200.f);
  REQUIRE(guest.IsDead());
  REQUIRE(guest.LastKillerId() == std::optional<uint32_t>(0));
  Leave(p);
}

TEST_CASE("A trap hit from a guest, on the host's own body, on an NPC, from "
          "a reference without a table, out of the trap's reach, or past the "
          "trap's budget is refused",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the guest does not host the temple
  Hit(p, 1, kWall, kHostActor, 75.f);
  REQUIRE(Health(p, kHostActor) == 1.f);
  // the host's own body is its own report's
  Hit(p, 0, kWall, kHostActor, 75.f);
  REQUIRE(Health(p, kHostActor) == 1.f);
  // an NPC's values are the host's hosted state
  p.worldState.npcEnabled = true;
  REQUIRE_NOTHROW(p.worldState.GetFormAt<MpActor>(kAlvor));
  Hit(p, 0, kWall, kAlvor, 75.f);
  REQUIRE(Health(p, kAlvor) == 1.f);
  // the portcullis carries a script but no damage table
  Hit(p, 0, kPortcullis, kGuestActor, 75.f);
  REQUIRE(Health(p, kGuestActor) == 1.f);
  // the guest's record stands past the trap's reach
  auto& guest = p.worldState.GetFormAt<MpActor>(kGuestActor);
  guest.SetPos(kFarFromTheWall);
  Hit(p, 0, kWall, kGuestActor, 75.f);
  REQUIRE(Health(p, kGuestActor) == 1.f);
  guest.SetPos(kByTheWall);
  // the trap's budget counts every report, refused ones too: eight in a
  // second, then the ninth is dropped before the rule sees it
  for (int i = 0; i < 8; ++i) {
    Hit(p, 0, kWall, kGuestActor, 76.f);
  }
  Hit(p, 0, kWall, kGuestActor, 75.f);
  REQUIRE(Health(p, kGuestActor) == 1.f);
  REQUIRE(ChangeValuesTo(p, 1) == 0);
  Leave(p);
}
