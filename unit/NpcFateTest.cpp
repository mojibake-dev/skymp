#include "ActorFateMessage.h"
#include "CreateActorMessage.h"
#include "OpenContainerMessage.h"
#include "TestUtils.hpp"
#include "UpdatePropertyMessage.h"
#include <catch2/catch_all.hpp>
#include <cmath>

PartOne& GetPartOne();

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the server's
// roll is the one roll. A player's arrival loads the game files' actors in
// the grid squares around it (the ring the engine is about to attach) and
// rolls them at that player's level; their fates go to the player as
// ActorFate ahead of the engine's own roll; a square is fated once per
// player.

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kC1 = 0xff000d01;
// the forest's leveled predator placements (LvlAnimalForestPredator, the
// template chain to LVLN 0x1e7a0; lab/scenarios/b-cell-host.yaml): 0x85547
// stands at (123592, -80301, 10705) in Tamriel, cell 0x9810
constexpr uint32_t kPredator = 0x85547;
constexpr uint32_t kPredator2 = 0x85549;
const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };

template <class Message>
std::vector<Message> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<Message> out;
  for (auto& m : p.Messages()) {
    if (m.userId != user) {
      continue;
    }
    if (auto typed = dynamic_cast<Message*>(m.message.get())) {
      out.push_back(*typed);
    }
  }
  return out;
}

uint64_t FateNowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}
}

TEST_CASE("A player's arrival rolls the ring's leveled actors at its level "
          "and sends their fates once per square",
          "[NpcFate][espm]")
{
  auto& p = GetPartOne();
  // the loader skips NPCs unless the server setting says otherwise (the
  // test fixture's default, as every NPC test sets it)
  p.worldState.npcEnabled = true;
  DoConnect(p, 0);
  p.CreateActor(kC1, kForest, 0, kTamriel);
  p.SetUserActor(0, kC1);
  auto& player = p.worldState.GetFormAt<MpActor>(kC1);
  ActorValueRecord record;
  record.level = 25;
  player.SetActorValueRecord(record);
  p.Messages().clear();

  // the first square change (the tick's reach refresh) runs the fate pass
  p.TickCellHosts(FateNowMs());
  const auto fates = SentTo<ActorFateMessage>(p, 0);
  REQUIRE_FALSE(fates.empty());
  const ActorFateMessage::Fate* predator = nullptr;
  for (const auto& msg : fates) {
    for (const auto& f : msg.fates) {
      if (f.refrId == kPredator) {
        predator = &f;
      }
    }
  }
  REQUIRE(predator != nullptr);
  REQUIRE(predator->level == 25);
  REQUIRE(predator->baseId != 0);

  // the record: loaded by the pass, rolled at the player's level, the chain
  // through the LVLN, the pin the chain's first leveled pick
  auto& form = p.worldState.LookupFormByIdNoLoad(kPredator);
  REQUIRE(form);
  MpActor* actor = form->AsActor();
  REQUIRE(actor);
  REQUIRE(actor->GetFateLevel().has_value());
  REQUIRE(*actor->GetFateLevel() == 25);
  REQUIRE(actor->GetTemplateChain().size() >= 2);
  REQUIRE(actor->GetFateBaseId() == predator->baseId);
  REQUIRE(actor->GetFateBaseId() != actor->GetBaseId());

  // the same squares again: nothing new
  p.Messages().clear();
  p.TickCellHosts(FateNowMs() + 1'000);
  REQUIRE(SentTo<ActorFateMessage>(p, 0).empty());

  // the change form keeps the level
  REQUIRE(actor->GetChangeForm().fateLevel == std::optional<uint16_t>{ 25 });

  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
}

// thuum docs/verbs/npc-fate.md, ADR-048's seam (docs/research/m3/
// death-path.md section 2): a game-file actor's death is the server's word
// on every seat. The DeathStateContainer reaches the actor's host alone;
// the other listening games get the isDead property, the respawn's shape,
// so a puppet dies by the record and never by the host's relayed sample.
TEST_CASE("A game-file actor's death is told to every listening game as the "
          "isDead property",
          "[NpcFate][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kViewer = 0xff000d02;
  DoConnect(p, 0);
  p.CreateActor(kViewer, kForest, 0, kTamriel);
  p.SetUserActor(0, kViewer);
  p.TickCellHosts(FateNowMs());
  auto& form = p.worldState.LookupFormByIdNoLoad(kPredator);
  REQUIRE(form);
  MpActor* actor = form->AsActor();
  REQUIRE(actor);
  auto& viewer = p.worldState.GetFormAt<MpActor>(kViewer);
  REQUIRE(actor->GetListeners().count(&viewer) == 1);
  if (actor->IsDead()) {
    actor->Respawn(false);
  }
  p.worldState.fateRespawnDue.erase(kPredator);
  p.Messages().clear();

  actor->Kill(nullptr);

  bool told = false;
  for (const auto& msg : SentTo<UpdatePropertyMessage>(p, 0)) {
    if (msg.idx == actor->GetIdx() && msg.propName == "isDead" &&
        msg.dataDump == "true") {
      told = true;
    }
  }
  REQUIRE(told);

  // the shared world: the body stands again and leaves no deadline behind
  actor->Respawn(false);
  p.worldState.fateRespawnDue.erase(kPredator);
  DoDisconnect(p, 0);
  p.DestroyActor(kViewer);
}

// thuum docs/verbs/npc-fate.md (the deaths, D3 A): a game-file actor's body
// keeps to the engine's reset clock. The death stamps the record with the
// server's game day; the body lies, dead, until FateRespawnHours later;
// then it stands at its placement, alive, its roll fresh and its fate sent
// again. The clock is the test's: read it before arming, tick inside the
// window and past it (CellHostTest's margins lesson).
TEST_CASE("A game-file actor's body lies until the reset hours pass, then "
          "stands at its placement rolled anew",
          "[NpcFate][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kHunter = 0xff000d03;
  DoConnect(p, 0);
  p.CreateActor(kHunter, kForest, 0, kTamriel);
  p.SetUserActor(0, kHunter);
  p.TickCellHosts(FateNowMs());
  auto& form = p.worldState.LookupFormByIdNoLoad(kPredator2);
  REQUIRE(form);
  MpActor* actor = form->AsActor();
  REQUIRE(actor);
  REQUIRE(actor->IsFateRespawned());
  REQUIRE(actor->MayDie(nullptr));
  REQUIRE(actor->GetFateLevel().has_value());
  if (actor->IsDead()) {
    actor->Respawn(false);
  }
  p.worldState.fateRespawnDue.erase(kPredator2);

  const auto savedClock = p.worldState.gameTime;
  float day = 100.f;
  p.worldState.gameTime = [&day] {
    GameTimeNow t;
    t.daysPassed = day;
    return t;
  };
  const float hours = p.worldState.FateRespawnHours();
  REQUIRE(hours > 0.f);

  // the body falls away from the placement
  const auto placement = actor->GetEditorLocationalData();
  actor->SetPos({ placement.pos.x + 300.f, placement.pos.y, placement.pos.z });
  p.Messages().clear();
  actor->Kill(nullptr);
  REQUIRE(actor->IsDead());
  REQUIRE(actor->GetChangeForm().fateDiedAt.has_value());
  REQUIRE(*actor->GetChangeForm().fateDiedAt == 100.f);
  REQUIRE(p.worldState.fateRespawnDue.count(kPredator2) == 1);

  // inside the window: the body lies
  day = 100.f + hours / 24.f - 0.01f;
  p.TickFateRespawns();
  REQUIRE(actor->IsDead());
  REQUIRE(p.worldState.fateRespawnDue.count(kPredator2) == 1);

  // past it: alive at the placement, the stamp cleared, the fate sent again
  day = 100.f + hours / 24.f + 0.01f;
  p.Messages().clear();
  p.TickFateRespawns();
  REQUIRE_FALSE(actor->IsDead());
  REQUIRE(std::abs(actor->GetPos().x - placement.pos.x) < 1.f);
  REQUIRE_FALSE(actor->GetChangeForm().fateDiedAt.has_value());
  REQUIRE(p.worldState.fateRespawnDue.count(kPredator2) == 0);
  REQUIRE(actor->GetTemplateChain().size() >= 2);
  REQUIRE_FALSE(SentTo<ActorFateMessage>(p, 0).empty());

  p.worldState.gameTime = savedClock;
  DoDisconnect(p, 0);
  p.DestroyActor(kHunter);
}

// thuum docs/verbs/npc-fate.md (the deaths, D13): the reset hours are the
// master's GMST iHoursToRespawnCell (240 in Skyrim.esm, lab/esm.py on sky-srv
// 2026-10-23), never the executable's compiled 72, unless the server setting
// npcRespawnHours speaks
TEST_CASE("The reset hours come from the master's GMST unless the server "
          "setting speaks",
          "[NpcFate][espm]")
{
  auto& p = GetPartOne();
  const float saved = p.worldState.npcRespawnHours;
  p.worldState.npcRespawnHours = 0.f;
  REQUIRE(p.worldState.FateRespawnHours() == 240.f);
  p.worldState.npcRespawnHours = 1.f;
  REQUIRE(p.worldState.FateRespawnHours() == 1.f);
  p.worldState.npcRespawnHours = saved;
}

// thuum docs/verbs/npc-fate.md (the deaths): a dead game-file actor is a
// container to the player beside it, the CONT branch's body (occupancy
// within 512, the record's inventory sent, OpenContainer), so the take and
// the put move items on the server. A live actor's activation opens nothing
// on the server (dialogue is the host's engine's).
TEST_CASE("A dead game-file actor is a container to the player beside it",
          "[NpcFate][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kLooter = 0xff000d04;
  constexpr uint32_t kGold = 0xf;
  DoConnect(p, 0);
  p.CreateActor(kLooter, kForest, 0, kTamriel);
  p.SetUserActor(0, kLooter);
  p.TickCellHosts(FateNowMs());
  auto& form = p.worldState.LookupFormByIdNoLoad(kPredator);
  REQUIRE(form);
  MpActor* actor = form->AsActor();
  REQUIRE(actor);
  if (actor->IsDead()) {
    actor->Respawn(false);
  }
  p.worldState.fateRespawnDue.erase(kPredator);
  auto& looter = p.worldState.GetFormAt<MpActor>(kLooter);
  looter.SetPos(actor->GetPos());

  // alive: nothing opens on the server
  p.Messages().clear();
  actor->Activate(looter);
  for (const auto& msg : SentTo<UpdatePropertyMessage>(p, 0)) {
    REQUIRE_FALSE(msg.idx == actor->GetIdx() && msg.propName == "inventory");
  }
  REQUIRE(SentTo<OpenContainerMessage>(p, 0).empty());

  // dead: the body opens as a container and the take is the server's
  actor->Kill(nullptr);
  REQUIRE(actor->IsDead());
  actor->AddItem(kGold, 3);
  p.Messages().clear();
  actor->Activate(looter);
  bool inventorySent = false;
  for (const auto& msg : SentTo<UpdatePropertyMessage>(p, 0)) {
    if (msg.idx == actor->GetIdx() && msg.propName == "inventory") {
      inventorySent = true;
    }
  }
  REQUIRE(inventorySent);
  REQUIRE_FALSE(SentTo<OpenContainerMessage>(p, 0).empty());
  Inventory::Entry gold;
  gold.baseId = kGold;
  gold.count = 3;
  actor->TakeItem(looter, gold);
  REQUIRE(looter.GetInventory().GetItemCount(kGold) == 3);
  REQUIRE(actor->GetInventory().GetItemCount(kGold) == 0);

  actor->Respawn(false);
  p.worldState.fateRespawnDue.erase(kPredator);
  DoDisconnect(p, 0);
  p.DestroyActor(kLooter);
}
