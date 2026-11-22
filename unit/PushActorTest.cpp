#include "ActionListener.h"
#include "CellLoadedMessage.h"
#include "PushActorMessage.h"
#include "SpSnippetMessage.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>

PartOne& GetPartOne();

// A script's push in the sender's game onto an actor another seat simulates
// is reported (PushActor), checked for consistency alone and run by that
// actor's host, PushActorAway with the caster's figure as self. The push
// rides on a hit: a cast of the caster's must have claimed one on the target. The hosting
// idiom is ActorDeathTest.cpp's (a seat's CellLoaded makes it the cell's host
// past the grace); the actor is its predator 0x85547 in the same forest cell.

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kShouter = 0xff000e06;
constexpr uint32_t kHost = 0xff000e07;
constexpr uint32_t kNpc = 0x85547;
constexpr uint32_t kForestCell = 0x9810;
const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };
// Unrelenting Force's first word's spell, VoiceUnrelentingForce1 (Skyrim.esm)
constexpr uint32_t kFus = 0x00013e09;

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

MpActor& Arrive(PartOne& p, Networking::UserId user, uint32_t id)
{
  DoConnect(p, user);
  p.CreateActor(id, kForest, 0, kTamriel);
  p.SetUserActor(user, id);
  p.TickCellHosts(NowMs());
  return p.worldState.GetFormAt<MpActor>(id);
}

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

void Push(PartOne& p, Networking::UserId user, uint32_t caster,
          uint32_t target, float force)
{
  RawMessageData raw;
  raw.userId = user;
  PushActorMessage msg;
  msg.caster = caster;
  msg.target = target;
  msg.force = force;
  p.GetActionListener().OnPushActor(raw, msg);
  // SpSnippet::Execute sends deferred (SendToUserDeferred): the snippet
  // reaches the messages at the next tick, as DropItemTest's drop does
  p.Tick();
}

// the PushActorAway snippets user `to` was sent since the last clear
int PushSnippetsTo(PartOne& p, Networking::UserId to, uint32_t self)
{
  int n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId != to || m.j["t"] != MsgType::SpSnippet) {
      continue;
    }
    if (m.j["class"] == "ObjectReference" &&
        m.j["function"] == "PushActorAway" && m.j["selfId"] == self) {
      ++n;
    }
  }
  return n;
}
}

TEST_CASE("A reported push onto an NPC another seat hosts is run by that host "
          "with the caster's figure as self",
          "[PushActor][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& shouter = Arrive(p, 0, kShouter);
  auto& host = Arrive(p, 1, kHost);
  auto& form = p.worldState.LookupFormByIdNoLoad(kNpc);
  REQUIRE(form);
  MpActor* npc = form->AsActor();
  REQUIRE(npc);
  if (npc->IsDead()) {
    npc->Respawn(false);
  }
  npc->SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  Host(p, 1, kForestCell);
  REQUIRE(p.worldState.HostOf(*npc) == &host);

  // no hit of the shouter's on the NPC behind it: refused
  p.Messages().clear();
  Push(p, 0, kShouter, kNpc, 15.f);
  REQUIRE(PushSnippetsTo(p, 1, kShouter) == 0);

  // its shout's hit on the NPC, claimed: the push runs on the host
  p.StartCast(kShouter, kFus, 2, kForest);
  REQUIRE(p.ClaimCast(kShouter, kFus, kNpc, npc->GetPos()).check ==
          PartOne::CastCheck::Claimed);
  p.Messages().clear();
  Push(p, 0, kShouter, kNpc, 15.f);
  REQUIRE(PushSnippetsTo(p, 1, kShouter) == 1);
  REQUIRE(PushSnippetsTo(p, 0, kShouter) == 0);

  // the sender's push of an actor it simulates itself is no report: refused
  p.Messages().clear();
  Push(p, 1, kHost, kNpc, 15.f);
  REQUIRE(PushSnippetsTo(p, 1, kHost) == 0);

  // a caster the sender neither is nor hosts: refused
  p.Messages().clear();
  Push(p, 0, kHost, kNpc, 15.f);
  REQUIRE(PushSnippetsTo(p, 1, kHost) == 0);

  // a player as the target: its throw is its own game's, refused
  p.Messages().clear();
  Push(p, 0, kShouter, kHost, 15.f);
  REQUIRE(PushSnippetsTo(p, 1, kShouter) == 0);

  // an NPC out of reach: refused
  npc->SetPos(kForest + NiPoint3{ 20000.f, 0.f, 0.f });
  p.Messages().clear();
  Push(p, 0, kShouter, kNpc, 15.f);
  REQUIRE(PushSnippetsTo(p, 1, kShouter) == 0);

  (void)shouter;
  p.DestroyActor(kShouter);
  p.DestroyActor(kHost);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}
