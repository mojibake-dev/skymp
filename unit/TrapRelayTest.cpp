#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>

PartOne& GetPartOne();

// thuum docs/verbs/traps.md (M4): the cell's host's linked activations. Bleak
// Falls Temple 0x371de: the spike wall 0x54b15's one activation parent is the
// plate 0x567f2 (unit/ActivateParentTest), the portcullis 0x3b9f1's name the
// lever 0x6a9e2 (lab/esm.py 2026-11-01, XAPR e2a90600). The plate stands at
// (8589, -5338, -2054), the portcullis at (4992, -1408, -768). The portcullis
// is an ACTI (NorPortcullisLarge01 0x3e726), not a DOOR: its rise is each
// engine's script on the relayed activation, and the server keeps no open
// state for it (pipeline 4392 read IsOpen false after the lever's report).
namespace {
constexpr uint32_t kHostActor = 0xff000fe1;
constexpr uint32_t kGuestActor = 0xff000fe2;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPlate = 0x567f2;
constexpr uint32_t kWall = 0x54b15;
constexpr uint32_t kLever = 0x6a9e2;
constexpr uint32_t kPortcullis = 0x3b9f1;
// the puzzle room (ADR-059 decision 2): the lever LeverPushAnimatingNoPlayer
// 0xaa8c3 carries defaultPillarPuzzleLever, whose properties name the dart
// trap 0x3720f (refActOnFailure02) and the marker 0x3e728
// (puzzleDoorActivator), the marker's activation parents the portcullis
// (XAPR); the marker is a STAT reference the server never loads
constexpr uint32_t kPuzzleLever = 0xaa8c3;
constexpr uint32_t kDart = 0x3720f;
constexpr uint32_t kMarker = 0x3e728;
const NiPoint3 kByThePlate{ 8589.f, -5438.f, -2054.f };
const NiPoint3 kByThePortcullis{ 4992.f, -1508.f, -768.f };
const NiPoint3 kByThePuzzleLever{ 5002.f, -1003.f, -754.f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// `user` hosts `cell` (ActorDeathTest's shape): any earlier host's grace
// passes first, then the loaded report grants the cell
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

// both players at `pos`, loaded references around them subscribed
void Stand(PartOne& p, const NiPoint3& pos)
{
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(pos);
    actor.ForceSubscriptionsUpdate();
  }
}

void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kByThePlate, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kByThePlate, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  // the targets loaded (their activation parents fill the table at Init)
  p.worldState.GetFormAt<MpObjectReference>(kWall);
  p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  Stand(p, kByThePlate);
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

// a game's report of its engine's linked activation (activationService)
void Linked(PartOne& p, Networking::UserId user, uint32_t caster,
            uint32_t target)
{
  ActivateMessage msg;
  msg.data.caster = caster;
  msg.data.target = target;
  msg.data.isSecondActivation = false;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnActivate(raw, msg);
}

std::vector<RelayedActivationMessage> RelayedTo(PartOne& p,
                                                Networking::UserId user)
{
  std::vector<RelayedActivationMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<RelayedActivationMessage*>(m.message.get());
    if (m.userId == user && typed) {
      out.push_back(*typed);
    }
  }
  return out;
}
}

TEST_CASE("The host's linked activation is recorded through the reference's "
          "own path and relayed to the cell's other game",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the plate fires the spike wall: relayed to the guest, not the host
  Linked(p, 0, kPlate, kWall);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kWall);
  REQUIRE(RelayedTo(p, 1)[0].caster == kPlate);
  REQUIRE(RelayedTo(p, 0).empty());
  // the lever's activation of the portcullis (an ACTI: the rise is the
  // engines' own on the relay) reaches the guest with the lever as caster
  Stand(p, kByThePortcullis);
  p.Messages().clear();
  Linked(p, 0, kLever, kPortcullis);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kPortcullis);
  REQUIRE(RelayedTo(p, 1)[0].caster == kLever);
  Leave(p);
}

TEST_CASE("A linked activation from a guest, an unlinked pair or past the "
          "budget is refused",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the guest does not host the cell
  Linked(p, 1, kPlate, kWall);
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());
  // the plate is no activation parent of the portcullis
  Linked(p, 0, kPlate, kPortcullis);
  REQUIRE(RelayedTo(p, 1).empty());
  // eight in a second pass, the ninth is dropped
  for (int i = 0; i < 8; ++i) {
    Linked(p, 0, kPlate, kWall);
  }
  REQUIRE(RelayedTo(p, 1).size() == 8);
  Linked(p, 0, kPlate, kWall);
  REQUIRE(RelayedTo(p, 1).size() == 8);
  Leave(p);
}

TEST_CASE("The puzzle lever's script properties are links, and a marker the "
          "server never loads is a caster by its id (ADR-059 decision 2)",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the lever's own Init fills its links; the dart is an ACTI the loader takes
  p.worldState.GetFormAt<MpObjectReference>(kPuzzleLever);
  p.worldState.GetFormAt<MpObjectReference>(kDart);
  Stand(p, kByThePuzzleLever);
  p.Messages().clear();
  // the failure's dart trap, linked by refActOnFailure02
  Linked(p, 0, kPuzzleLever, kDart);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kDart);
  REQUIRE(RelayedTo(p, 1)[0].caster == kPuzzleLever);
  // the marker as a target is no form of the server's: refused, no relay (its
  // consequence is the next report)
  p.Messages().clear();
  Linked(p, 0, kPuzzleLever, kMarker);
  REQUIRE(RelayedTo(p, 1).empty());
  // the marker as the caster of the portcullis's activation: the portcullis's
  // XAPR names it, so the link holds by id although the marker never loads
  Stand(p, kByThePortcullis);
  p.Messages().clear();
  Linked(p, 0, kMarker, kPortcullis);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kPortcullis);
  REQUIRE(RelayedTo(p, 1)[0].caster == kMarker);
  // a stranger's id that is no reference at all is still refused
  p.Messages().clear();
  Linked(p, 0, 0x12345, kPortcullis);
  REQUIRE(RelayedTo(p, 1).empty());
  Leave(p);
}
