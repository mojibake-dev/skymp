#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <vector>

PartOne& GetPartOne();

// An act on a world object goes to the object's owner, the game whose engine
// runs its scripts (its cell's host), wherever that game's player stands in
// the cell, and to the object's other listeners. Each case has the guest act
// alone in Bleak Falls Temple 0x371de while the host's player stands by the
// temple's entrance, out of the object's listeners: the spike wall 0x54b15 its
// plate 0x567f2 fires, the lamp 0x9b0fb a guest's arrow brings down, the
// puzzle lever 0xaa8c3 the guest pulls.
namespace {
constexpr uint32_t kOwnerHost = 0xff000f71;
constexpr uint32_t kOwnerGuest = 0xff000f72;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPlate = 0x567f2;
constexpr uint32_t kWall = 0x54b15;
constexpr uint32_t kLamp = 0x9b0fb;
constexpr uint32_t kLever = 0xaa8c3;
const NiPoint3 kByThePlate{ 8589.f, -5438.f, -2054.f };
// by the temple's entrance, out of the listeners of every object below
const NiPoint3 kAtTheEntrance{ -4384.f, 1163.f, 577.f };

uint64_t OwnerNowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

void Loaded(PartOne& p, Networking::UserId user, uint32_t cell)
{
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage loaded;
  loaded.cell = cell;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
}

// the host (user 0) hosts the temple and the guest (user 1) has it loaded,
// the guest at `guestAt`, the host's player at the entrance
void Enter(PartOne& p, const NiPoint3& guestAt)
{
  DoConnect(p, 0);
  p.CreateActor(kOwnerHost, guestAt, 0, kTemple);
  p.SetUserActor(0, kOwnerHost);
  DoConnect(p, 1);
  p.CreateActor(kOwnerGuest, guestAt, 0, kTemple);
  p.SetUserActor(1, kOwnerGuest);
  for (uint32_t id : { kOwnerHost, kOwnerGuest }) {
    p.worldState.GetFormAt<MpActor>(id).ForceSubscriptionsUpdate();
  }
  const auto past = OwnerNowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  Loaded(p, 0, kTemple);
  p.TickCellHosts(past);
  Loaded(p, 1, kTemple);
  REQUIRE(p.GetCellHost(kTemple).player == kOwnerHost);
  auto& host = p.worldState.GetFormAt<MpActor>(kOwnerHost);
  host.SetPos(kAtTheEntrance);
  host.ForceSubscriptionsUpdate();
  REQUIRE(p.GetCellHost(kTemple).player == kOwnerHost);
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kOwnerHost);
  p.DestroyActor(kOwnerGuest);
}

bool Listens(PartOne& p, MpObjectReference& ref, uint32_t actorId)
{
  auto& actor = p.worldState.GetFormAt<MpActor>(actorId);
  const auto& listeners = ref.GetActorListeners();
  return std::find(listeners.begin(), listeners.end(), &actor) !=
    listeners.end();
}

template <class M>
std::vector<M> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<M> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<M*>(m.message.get());
    if (m.userId == user && typed) {
      out.push_back(*typed);
    }
  }
  return out;
}
}

TEST_CASE("A guest's linked activation of a world object reaches the cell's "
          "host though the host's player stands across the temple",
          "[OwnerRelay][espm]")
{
  PartOne& p = GetPartOne();
  auto& wall = p.worldState.GetFormAt<MpObjectReference>(kWall);
  Enter(p, kByThePlate);
  REQUIRE_FALSE(Listens(p, wall, kOwnerHost));
  ActivateMessage msg;
  msg.data.caster = kPlate;
  msg.data.target = kWall;
  msg.data.isSecondActivation = false;
  RawMessageData raw;
  raw.userId = 1;
  p.GetActionListener().OnActivate(raw, msg);
  const auto toHost = SentTo<RelayedActivationMessage>(p, 0);
  REQUIRE(toHost.size() == 1);
  REQUIRE(toHost[0].target == kWall);
  REQUIRE(toHost[0].caster == kPlate);
  REQUIRE(SentTo<RelayedActivationMessage>(p, 1).empty());
  Leave(p);
}

TEST_CASE("A guest's destruction stage of a world object reaches the cell's "
          "host though the host's player stands across the temple",
          "[OwnerRelay][espm]")
{
  PartOne& p = GetPartOne();
  auto& lamp = p.worldState.GetFormAt<MpObjectReference>(kLamp);
  Enter(p, lamp.GetPos() + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE_FALSE(Listens(p, lamp, kOwnerHost));
  DestructionStageMessage msg;
  msg.refr = kLamp;
  msg.stage = 1;
  msg.destroyed = false;
  msg.served = false;
  RawMessageData raw;
  raw.userId = 1;
  p.GetActionListener().OnDestructionStage(raw, msg);
  const auto toHost = SentTo<DestructionStageMessage>(p, 0);
  REQUIRE(toHost.size() == 1);
  REQUIRE(toHost[0].refr == kLamp);
  REQUIRE(toHost[0].stage == 1);
  REQUIRE(SentTo<DestructionStageMessage>(p, 1).empty());
  Leave(p);
}

TEST_CASE("A furniture's guest user's graph event reaches the cell's host "
          "though the host's player stands across the temple",
          "[OwnerRelay][espm]")
{
  PartOne& p = GetPartOne();
  auto& lever = p.worldState.GetFormAt<MpObjectReference>(kLever);
  Enter(p, lever.GetPos() + NiPoint3{ -60.f, 0.f, 0.f });
  REQUIRE_FALSE(Listens(p, lever, kOwnerHost));
  auto& guest = p.worldState.GetFormAt<MpActor>(kOwnerGuest);
  lever.Activate(guest);
  REQUIRE(lever.GetOccupant() == &guest);
  p.Messages().clear();
  ObjectAnimationMessage msg;
  msg.refr = kLever;
  msg.event = "Lever_PlayerPush";
  RawMessageData raw;
  raw.userId = 1;
  p.GetActionListener().OnObjectAnimation(raw, msg);
  const auto toHost = SentTo<ObjectAnimationMessage>(p, 0);
  REQUIRE(toHost.size() == 1);
  REQUIRE(toHost[0].refr == kLever);
  REQUIRE(toHost[0].event == "Lever_PlayerPush");
  REQUIRE(SentTo<ObjectAnimationMessage>(p, 1).empty());
  Leave(p);
}
