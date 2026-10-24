#include "ActorFateMessage.h"
#include "CreateActorMessage.h"
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
    actor->SetIsDead(false);
  }
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

  actor->SetIsDead(false);
  DoDisconnect(p, 0);
  p.DestroyActor(kViewer);
}
