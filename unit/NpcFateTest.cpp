#include "ActionListener.h"
#include "ActorFateMessage.h"
#include "ConsoleCommandMessage.h"
#include "CreateActorMessage.h"
#include "MpChangeForms.h"
#include "OpenContainerMessage.h"
#include "TestUtils.hpp"
#include "UpdatePropertyMessage.h"
#include "libespm/Loader.h"
#include "libespm/NPC_.h"
#include <catch2/catch_all.hpp>
#include <cmath>
#include <simdjson.h>

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
    REQUIRE_FALSE((msg.idx == actor->GetIdx() && msg.propName == "inventory"));
  }
  REQUIRE(SentTo<OpenContainerMessage>(p, 0).empty());

  // dead: the body opens as a container and the take is the server's
  actor->Kill(nullptr);
  REQUIRE(actor->IsDead());
  actor->AddItem(kGold, 3);
  // the shared world: neither inventory starts empty (the looter held 140
  // gold in CI), so the take is read as a difference
  const auto corpseGold = actor->GetInventory().GetItemCount(kGold);
  const auto looterGold = looter.GetInventory().GetItemCount(kGold);
  REQUIRE(corpseGold >= 3);
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
  REQUIRE(looter.GetInventory().GetItemCount(kGold) == looterGold + 3);
  REQUIRE(actor->GetInventory().GetItemCount(kGold) == corpseGold - 3);

  actor->Respawn(false);
  p.worldState.fateRespawnDue.erase(kPredator);
  DoDisconnect(p, 0);
  p.DestroyActor(kLooter);
}

// thuum docs/verbs/npc-fate.md (the deaths): the body's day is on the change
// form and survives its JSON (the restart's record); an older record without
// it reads as "no stamp"
TEST_CASE("A body's day survives the change form's JSON", "[NpcFate]")
{
  MpChangeForm changeForm;
  changeForm.isDead = true;
  changeForm.fateDiedAt = 100.5f;
  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  const auto back = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(back.isDead);
  REQUIRE(back.fateDiedAt == std::optional<float>{ 100.5f });

  const std::string older = MpChangeForm::ToJson(MpChangeForm()).dump();
  REQUIRE(older.find("fateDiedAt") == std::string::npos);
  auto olderElement = parser.parse(older).value();
  REQUIRE_FALSE(
    MpChangeForm::JsonToChangeForm(olderElement).fateDiedAt.has_value());
}

// thuum docs/verbs/npc-fate.md (D15): the engine's rule for a game-file base,
// read from the master's own flags: essential never dies, protected dies to
// a player's hand alone. Hod 0x1348a (Riverwood's persistent mill worker,
// CellHostTest's espm fact) is whatever Skyrim.esm says he is: the rule is
// asserted against the flags, never a remembered flag.
TEST_CASE("A game-file base dies by the master's essential and protected "
          "flags",
          "[NpcFate][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kHodRef = 0x1348a;
  constexpr uint32_t kHand = 0xff000d05;
  DoConnect(p, 0);
  p.CreateActor(kHand, kForest, 0, kTamriel);
  p.SetUserActor(0, kHand);
  auto& hand = p.worldState.GetFormAt<MpActor>(kHand);
  auto& form = p.worldState.LookupFormById(kHodRef);
  REQUIRE(form);
  MpActor* hod = form->AsActor();
  REQUIRE(hod);
  const auto flags =
    espm::GetData<espm::NPC_>(hod->GetBaseId(), &p.worldState);
  REQUIRE(hod->MayDie(nullptr) == !(flags.isEssential || flags.isProtected));
  REQUIRE(hod->MayDie(&hand) == !flags.isEssential);

  // a leveled pick carries neither flag: it dies to anyone
  auto& predatorForm = p.worldState.LookupFormByIdNoLoad(kPredator);
  REQUIRE(predatorForm);
  REQUIRE(predatorForm->AsActor()->MayDie(nullptr));

  DoDisconnect(p, 0);
  p.DestroyActor(kHand);
}

// thuum docs/verbs/npc-fate.md (the deaths), playtest 34 (2026-10-25): two
// server-made bandits placed by the lab in Bleak Falls Temple (a 0xff id,
// an NPC base, profileId -1) died to headshots and stood again 25 s later
// by the players' rule, while the game-file bandit beside them lay for the
// reset hours. A server-made actor with an NPC base is fated like a
// game-file one: the death schedules the reset clock, no wall timer.
TEST_CASE("A server-made actor with an NPC base dies by the NPC rule, not "
          "the players' 25 s respawn",
          "[NpcFate][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kMade = 0xff000d0c;
  constexpr uint32_t kBanditBase = 0x3de6f; // EncBandit01Melee2HBerserkNordM
  MpChangeForm saved;
  saved.recType = MpChangeForm::ACHR;
  saved.formDesc = FormDesc::FromFormId(kMade, p.worldState.espmFiles);
  saved.baseDesc = FormDesc::FromFormId(kBanditBase, p.worldState.espmFiles);
  saved.worldOrCellDesc = FormDesc::Tamriel();
  saved.position = kForest;
  saved.angle = { 0.f, 0.f, 0.f };
  // an ACHR change form waits for the deferred load (unit/NpcExists.cpp's
  // idiom), then stands on the grid at its saved position
  p.worldState.LoadChangeForm(saved, p.CreateFormCallbacks());
  p.worldState.LoadDeferredActors();
  auto& form = p.worldState.LookupFormByIdNoLoad(kMade);
  REQUIRE(form);
  MpActor* made = form->AsActor();
  REQUIRE(made);
  REQUIRE(made->GetBaseId() == kBanditBase);
  REQUIRE_FALSE(made->IsCreatedAsPlayer());
  REQUIRE(made->IsFateRespawned());
  p.worldState.fateRespawnDue.erase(kMade);

  made->Kill(nullptr);
  REQUIRE(made->IsDead());
  // the reset clock, not the 25 s timer
  REQUIRE(p.worldState.fateRespawnDue.count(kMade) == 1);
  REQUIRE(made->GetChangeForm().fateDiedAt.has_value());

  // the shared world
  made->Respawn(false);
  p.worldState.fateRespawnDue.erase(kMade);
  p.DestroyActor(kMade);
}

// thuum ADR-050 decision 3 (train 51): a server-made NPC goes through the
// same roll and pin as a game-file actor, keyed before PlaceAtMe returns
TEST_CASE("A server-made NPC is fated at its placement, its fate goes ahead "
          "of its create word, and its death is the server's word to every "
          "listener",
          "[NpcFate][espm]")
{
  enum
  {
    EncBandit01Melee1HNordM = 0x1bcd8,
    kC2 = 0xff000d02
  };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  DoConnect(p, 0);
  p.CreateActor(kC1, kForest, 0, kTamriel);
  p.SetUserActor(0, kC1);
  auto& player = p.worldState.GetFormAt<MpActor>(kC1);
  ActorValueRecord record;
  record.level = 25;
  player.SetActorValueRecord(record);
  player.SetConsoleCommandsAllowedFlag(true);
  // a second game in the same square hears the words too
  DoConnect(p, 1);
  p.CreateActor(kC2, kForest + NiPoint3{ 200.f, 0.f, 0.f }, 0, kTamriel);
  p.SetUserActor(1, kC2);
  p.Messages().clear();

  RawMessageData msgData;
  msgData.userId = 0;
  ConsoleCommandMessage cmd;
  cmd.data.commandName = "placeatme";
  cmd.data.args = { int64_t(0x14), int64_t(EncBandit01Melee1HNordM) };
  p.GetActionListener().OnConsoleCommand(msgData, cmd);

  // the placed actor is named by the one fate with that base under a 0xff
  // id; its create word to the placer follows the fate
  uint32_t placedId = 0;
  size_t fateAt = 0;
  for (size_t i = 0; i < p.Messages().size(); ++i) {
    auto& m = p.Messages()[i];
    if (m.userId != 0) {
      continue;
    }
    if (auto fate = dynamic_cast<ActorFateMessage*>(m.message.get())) {
      for (const auto& f : fate->fates) {
        if (f.refrId >= 0xff000000 && f.baseId == EncBandit01Melee1HNordM) {
          placedId = f.refrId;
          fateAt = i;
        }
      }
    }
  }
  REQUIRE(placedId >= 0xff000000);
  auto& placed = p.worldState.GetFormAt<MpActor>(placedId);
  REQUIRE_FALSE(placed.IsCreatedAsPlayer());
  REQUIRE(placed.GetFateLevel().has_value());
  REQUIRE(*placed.GetFateLevel() == 25);
  // what the record wears, its inventory holds (one roll for the outfit): the
  // host's hands report of the copy passes the inventory check
  for (const auto& worn : placed.GetEquipment().inv.entries) {
    if (worn.GetWorn() != Inventory::Worn::None) {
      INFO("worn " << std::hex << worn.baseId);
      REQUIRE(placed.GetInventory().GetItemCount(worn.baseId) >= 1);
    }
  }
  size_t createAt = 0;
  bool createSeen = false;
  for (size_t i = 0; i < p.Messages().size(); ++i) {
    auto& m = p.Messages()[i];
    if (m.userId != 0) {
      continue;
    }
    if (auto create = dynamic_cast<CreateActorMessage*>(m.message.get())) {
      if (create->refrId.has_value() && *create->refrId == placedId) {
        createSeen = true;
        createAt = i;
      }
    }
  }
  REQUIRE(createSeen);
  REQUIRE(fateAt < createAt);
  // the second game got the fate ahead of its create word as well
  bool fateToSecond = false;
  for (auto& m : p.Messages()) {
    if (m.userId != 1) {
      continue;
    }
    if (auto fate = dynamic_cast<ActorFateMessage*>(m.message.get())) {
      for (const auto& f : fate->fates) {
        if (f.refrId == placedId) {
          fateToSecond = true;
        }
      }
    }
  }
  REQUIRE(fateToSecond);

  // the death is the server's word to the listener that does not host it
  p.Messages().clear();
  placed.Kill(nullptr);
  bool deathWord = false;
  for (auto& m : p.Messages()) {
    if (m.userId == 1 && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::UpdateProperty) &&
        m.j.value("propName", std::string()) == "isDead" &&
        m.j.value("refrId", 0u) == placedId &&
        m.j.value("dataDump", std::string()) == "true") {
      deathWord = true;
    }
  }
  REQUIRE(deathWord);

  p.DestroyActor(placedId);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}

// thuum docs/research/m3/decisions-after-playtest-34.md D21 (playtest 34,
// section 5, numidium's read): the ice wolf ff000003, disabled by the
// console at 23:00Z, was killed at 23:01:00 and stood again enabled at
// 23:01:25. A disabled actor is out of the world by the server's own word:
// its death schedules no return, a body disabled while it lies waits, and
// the enable resumes the clock (from the enable when the death left no
// stamp, from the death otherwise).
TEST_CASE("A disabled actor's death schedules no respawn, a body disabled "
          "meanwhile waits, and the enable resumes the clock",
          "[NpcFate][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kOut = 0xff000d0d;
  constexpr uint32_t kBanditBase = 0x3de6f; // EncBandit01Melee2HBerserkNordM
  MpChangeForm saved;
  saved.recType = MpChangeForm::ACHR;
  saved.formDesc = FormDesc::FromFormId(kOut, p.worldState.espmFiles);
  saved.baseDesc = FormDesc::FromFormId(kBanditBase, p.worldState.espmFiles);
  saved.worldOrCellDesc = FormDesc::Tamriel();
  saved.position = kForest;
  saved.angle = { 0.f, 0.f, 0.f };
  p.worldState.LoadChangeForm(saved, p.CreateFormCallbacks());
  p.worldState.LoadDeferredActors();
  auto& form = p.worldState.LookupFormByIdNoLoad(kOut);
  REQUIRE(form);
  MpActor* made = form->AsActor();
  REQUIRE(made);
  REQUIRE(made->IsFateRespawned());
  p.worldState.fateRespawnDue.erase(kOut);

  const auto savedClock = p.worldState.gameTime;
  float day = 200.f;
  p.worldState.gameTime = [&day] {
    GameTimeNow t;
    t.daysPassed = day;
    return t;
  };
  const float hours = p.worldState.FateRespawnHours();
  REQUIRE(hours > 0.f);

  // out of the world first: the death schedules nothing and stamps nothing
  made->Disable();
  REQUIRE(made->IsDisabled());
  made->Kill(nullptr);
  REQUIRE(made->IsDead());
  REQUIRE(p.worldState.fateRespawnDue.count(kOut) == 0);
  REQUIRE_FALSE(made->GetChangeForm().fateDiedAt.has_value());

  // the enable starts the clock from now
  made->Enable();
  REQUIRE_FALSE(made->IsDisabled());
  REQUIRE(p.worldState.fateRespawnDue.count(kOut) == 1);
  REQUIRE(made->GetChangeForm().fateDiedAt == std::optional<float>{ 200.f });

  // disabled while the body lies: the sweep drops it and keeps the stamp
  made->Disable();
  day = 200.f + hours / 24.f + 0.01f;
  p.TickFateRespawns();
  REQUIRE(made->IsDead());
  REQUIRE(p.worldState.fateRespawnDue.count(kOut) == 0);
  REQUIRE(made->GetChangeForm().fateDiedAt == std::optional<float>{ 200.f });

  // back in the world past its hours: the enable puts it on the clock and
  // the next sweep stands it
  made->Enable();
  REQUIRE(p.worldState.fateRespawnDue.count(kOut) == 1);
  p.TickFateRespawns();
  REQUIRE_FALSE(made->IsDead());
  REQUIRE(p.worldState.fateRespawnDue.count(kOut) == 0);

  // the shared world
  p.worldState.gameTime = savedClock;
  p.worldState.fateRespawnDue.erase(kOut);
  p.DestroyActor(kOut);
}
