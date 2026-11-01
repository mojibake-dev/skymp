#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>

PartOne& GetPartOne();

// thuum docs/verbs/traps.md (ADR-059 decision 3): the cell's host reports a
// world object's animation event; the server records it as the reference's
// lastAnimation and relays it to the cell's other games. The stage is
// TrapRelayTest's: Bleak Falls Temple 0x371de, the portcullis 0x3b9f1 (an
// ACTI, NorPortcullisLarge01) at (4992, -1408, -768), both players beside it.
namespace {
constexpr uint32_t kHostActor = 0xff000fe3;
constexpr uint32_t kGuestActor = 0xff000fe4;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPortcullis = 0x3b9f1;
constexpr uint32_t kBandit = 0x39fe4;
const NiPoint3 kByThePortcullis{ 4992.f, -1508.f, -768.f };

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
  p.CreateActor(kHostActor, kByThePortcullis, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kByThePortcullis, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(kByThePortcullis);
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

void Animate(PartOne& p, Networking::UserId user, uint32_t refr,
             const char* event)
{
  ObjectAnimationMessage msg;
  msg.refr = refr;
  msg.event = event;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnObjectAnimation(raw, msg);
}

std::vector<ObjectAnimationMessage> RelayedTo(PartOne& p,
                                              Networking::UserId user)
{
  std::vector<ObjectAnimationMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<ObjectAnimationMessage*>(m.message.get());
    if (m.userId == user && typed) {
      out.push_back(*typed);
    }
  }
  return out;
}

std::optional<std::string> LastAnimation(PartOne& p, uint32_t refr)
{
  return p.worldState.GetFormAt<MpObjectReference>(refr)
    .GetChangeForm()
    .lastAnimation;
}
}

TEST_CASE("The host's world-object animation is recorded as the reference's "
          "last animation and relayed to the cell's other game",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Animate(p, 0, kPortcullis, "Open");
  REQUIRE(LastAnimation(p, kPortcullis) == std::optional<std::string>("Open"));
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].refr == kPortcullis);
  REQUIRE(RelayedTo(p, 1)[0].event == "Open");
  REQUIRE(RelayedTo(p, 0).empty());
  // the next event replaces the record
  p.Messages().clear();
  Animate(p, 0, kPortcullis, "Close");
  REQUIRE(LastAnimation(p, kPortcullis) ==
          std::optional<std::string>("Close"));
  REQUIRE(RelayedTo(p, 1).size() == 1);
  Leave(p);
}

TEST_CASE("A world-object animation from a guest, on an actor, on no "
          "reference, or past the reference's budget is refused",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Animate(p, 0, kPortcullis, "Open");
  p.Messages().clear();
  // the guest does not host the temple
  Animate(p, 1, kPortcullis, "Close");
  REQUIRE(LastAnimation(p, kPortcullis) == std::optional<std::string>("Open"));
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());
  // an actor is no world object: a server-made one by its range, a game-file
  // one (MS13BanditCampfire01 0x39fe4, the temple's entrance bandit) as an
  // actor
  Animate(p, 0, kGuestActor, "Turned01");
  REQUIRE(RelayedTo(p, 1).empty());
  REQUIRE(!p.worldState.GetFormAt<MpActor>(kGuestActor)
             .GetChangeForm()
             .lastAnimation.has_value());
  Animate(p, 0, kBandit, "Turned01");
  REQUIRE(RelayedTo(p, 1).empty());
  REQUIRE(!p.worldState.GetFormAt<MpActor>(kBandit)
             .GetChangeForm()
             .lastAnimation.has_value());
  // a form the server never loads, and an empty event
  Animate(p, 0, 0x3e728, "Open");
  Animate(p, 0, kPortcullis, "");
  REQUIRE(RelayedTo(p, 1).empty());
  // the reference's budget: eight in a second pass, the ninth is dropped
  for (int i = 0; i < 7; ++i) {
    Animate(p, 0, kPortcullis, "Trigger01");
  }
  REQUIRE(RelayedTo(p, 1).size() == 7);
  Animate(p, 0, kPortcullis, "Trigger02");
  REQUIRE(RelayedTo(p, 1).size() == 7);
  REQUIRE(LastAnimation(p, kPortcullis) ==
          std::optional<std::string>("Trigger01"));
  Leave(p);
}
