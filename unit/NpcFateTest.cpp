#include "ActionListener.h"
#include "ActorDeathMessage.h"
#include "ActorFateMessage.h"
#include "ActorValueRecord.h"
#include "CellLoadedMessage.h"
#include "ConsoleCommandMessage.h"
#include "CreateActorMessage.h"
#include "HitMessage.h"
#include "HostedActorStateMessage.h"
#include "MpChangeForms.h"
#include "OpenContainerMessage.h"
#include "SpSnippet.h"
#include "TestUtils.hpp"
#include "UpdateEquipmentMessage.h"
#include "UpdatePropertyMessage.h"
#include "condition_functions/HasKeyword.h"
#include "libespm/Loader.h"
#include "libespm/MGEF.h"
#include "libespm/NPC_.h"
#include "libespm/SPEL.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <cmath>
#include <optional>
#include <simdjson.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

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

// the forest's cell, the predator's (NpcCastTest's)
constexpr uint32_t kForestCell = 0x9810;

const auto kWornRight = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();

// `user` hosts `cell`: any earlier host left at its case's end, so its grace
// passes first, then the loaded report grants the cell (NpcCastTest's shape)
void HostGrants(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = FateNowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage loaded;
  loaded.cell = cell;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  p.TickCellHosts(past);
}

// the raw bytes a relayed report carries: the message as the host's client
// would send it (SendToNeighbours forwards them as they came, and the
// fixture types the relay by them); kept in a static for the call
RawMessageData HostedRaw(const UpdateEquipmentMessage& msg)
{
  static std::string bytes;
  nlohmann::json j;
  msg.WriteJson(j);
  bytes.clear();
  bytes.push_back(static_cast<char>(Networking::MinPacketId));
  bytes += j.dump();
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = reinterpret_cast<Networking::PacketData>(bytes.data());
  raw.unparsedLength = bytes.size();
  return raw;
}

// the host's report of what `actor` wears: each entry a count of one, worn
void Wears(PartOne& p, MpActor& actor, const std::vector<uint32_t>& worn,
           uint32_t numChanges)
{
  UpdateEquipmentMessage msg;
  msg.idx = actor.GetIdx();
  msg.data.numChanges = numChanges;
  for (const auto baseId : worn) {
    msg.data.inv.entries.push_back(Inventory::Entry(baseId, 1, kWornRight));
  }
  const RawMessageData raw = HostedRaw(msg);
  p.GetActionListener().OnUpdateEquipment(raw, msg);
}

// the host's report of `aggressor`'s swing at `target` with `weapon`, the
// weapon's cooldown behind it (HitTest's SetLastHitTime idiom)
void Swings(PartOne& p, MpActor& aggressor, uint32_t target, uint32_t weapon)
{
  aggressor.SetLastHitTime(
    target, std::chrono::steady_clock::now() - std::chrono::seconds(10));
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = aggressor.GetFormId();
  hit.data.target = target;
  hit.data.source = weapon;
  p.GetActionListener().OnHit(raw, hit);
}

bool Wearing(const MpActor& actor, uint32_t baseId)
{
  for (const auto& entry : actor.GetEquipment().inv.entries) {
    if (entry.baseId == baseId && entry.GetWorn() != Inventory::Worn::None) {
      return true;
    }
  }
  return false;
}

float HealthOf(PartOne& p, uint32_t id)
{
  return p.worldState.GetFormAt<MpActor>(id)
    .GetChangeForm()
    .actorValues.healthPercentage;
}

size_t SentOfType(PartOne& p, Networking::UserId user, MsgType type)
{
  size_t n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(type)) {
      ++n;
    }
  }
  return n;
}

// `user`'s state sample of `actorId` (CellHostTest's State idiom): the
// health, the other percentages full, no target and no package
void State(PartOne& p, Networking::UserId user, uint32_t actorId, float health)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  HostedActorStateMessage msg;
  msg.idx = p.worldState.GetFormAt<MpActor>(actorId).GetIdx();
  msg.health = health;
  msg.magicka = 1.f;
  msg.stamina = 1.f;
  p.GetActionListener().OnHostedActorState(raw, msg);
}

// the fates naming `refrId` sent to `user` since the last clear
size_t FatesOf(PartOne& p, Networking::UserId user, uint32_t refrId)
{
  size_t n = 0;
  for (const auto& msg : SentTo<ActorFateMessage>(p, user)) {
    n += static_cast<size_t>(std::count_if(
      msg.fates.begin(), msg.fates.end(),
      [&](const ActorFateMessage::Fate& f) { return f.refrId == refrId; }));
  }
  return n;
}

// where the first fate naming `refrId`, and the first create word for it,
// stand among the messages sent to `user`: the order its game reads them in
std::optional<size_t> FirstFateAt(PartOne& p, Networking::UserId user,
                                  uint32_t refrId)
{
  auto& messages = p.Messages();
  for (size_t i = 0; i < messages.size(); ++i) {
    auto fate = dynamic_cast<ActorFateMessage*>(messages[i].message.get());
    if (messages[i].userId != user || !fate) {
      continue;
    }
    for (const auto& f : fate->fates) {
      if (f.refrId == refrId) {
        return i;
      }
    }
  }
  return std::nullopt;
}

std::optional<size_t> FirstCreateAt(PartOne& p, Networking::UserId user,
                                    uint32_t refrId)
{
  auto& messages = p.Messages();
  for (size_t i = 0; i < messages.size(); ++i) {
    auto create = dynamic_cast<CreateActorMessage*>(messages[i].message.get());
    if (messages[i].userId == user && create &&
        create->refrId == std::optional<uint64_t>(refrId)) {
      return i;
    }
  }
  return std::nullopt;
}

// the words naming the actor of `idx` by its idx (an equipment word, a
// property, a teleport) sent to `user` ahead of message `before`: what its
// game would read about it ahead of its create word there
size_t IdxWordsBefore(PartOne& p, Networking::UserId user, uint32_t idx,
                      size_t before)
{
  size_t n = 0;
  auto& messages = p.Messages();
  for (size_t i = 0; i < before && i < messages.size(); ++i) {
    const auto& m = messages[i];
    if (m.userId == user && m.j.is_object() && m.j.contains("idx") &&
        m.j["idx"] == idx) {
      ++n;
    }
  }
  return n;
}

// the first fate naming `refrId` among the messages sent to `user`
std::optional<ActorFateMessage::Fate> FirstFateOf(PartOne& p,
                                                  Networking::UserId user,
                                                  uint32_t refrId)
{
  const auto at = FirstFateAt(p, user, refrId);
  if (!at) {
    return std::nullopt;
  }
  auto fate = dynamic_cast<ActorFateMessage*>(p.Messages()[*at].message.get());
  for (const auto& f : fate->fates) {
    if (f.refrId == refrId) {
      return f;
    }
  }
  return std::nullopt;
}

// the server's log lines while it lives (StandingTest's idiom)
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("npc-fate-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~LogCapture() { spdlog::set_default_logger(old); }
  size_t Count(const std::string& line) const
  {
    const std::string log = oss.str();
    size_t n = 0;
    for (size_t at = log.find(line); at != std::string::npos;
         at = log.find(line, at + line.size())) {
      ++n;
    }
    return n;
  }
};
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
  // the level the player logs in with (a save's record): the login's create
  // words already carry the rolls of the actors in its 3x3 (EnsureFated)
  auto& player = p.worldState.GetFormAt<MpActor>(kC1);
  ActorValueRecord record;
  record.level = 25;
  player.SetActorValueRecord(record);
  p.SetUserActor(0, kC1);
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

  // the record: rolled at the player's level (at the login's create word,
  // the predator in the player's own square), the chain through the LVLN,
  // the pin the chain's first leveled pick
  auto& form = p.worldState.LookupFormByIdNoLoad(kPredator);
  REQUIRE(form);
  MpActor* actor = form->AsActor();
  REQUIRE(actor);
  REQUIRE(actor->GetFateLevel().has_value());
  REQUIRE(*actor->GetFateLevel() == 25);
  REQUIRE(actor->GetTemplateChain().size() >= 2);
  REQUIRE(actor->GetFateBaseId() == predator->baseId);
  REQUIRE(actor->GetFateBaseId() != actor->GetBaseId());

  // thuum docs/verbs/npc-fate.md (playtest 44's 1c): the fate carries the
  // record's inventory rows apart from the worn items: no row names a worn
  // base, and the rows' counts sum to the inventory less the worn (the
  // review of 7c3c9f79, finding 1: the inventory's entries carry no worn
  // flag of their own, the Equipment record does)
  {
    int64_t total = 0;
    for (const auto& e : actor->GetInventory().entries) {
      total += e.count;
    }
    int64_t worn = 0;
    for (const auto& e : actor->GetEquipment().inv.entries) {
      if (e.GetWorn() != Inventory::Worn::None) {
        worn += e.count;
      }
    }
    int64_t rows = 0;
    for (const auto& row : predator->inv) {
      REQUIRE(row.count > 0);
      REQUIRE(std::find(predator->worn.begin(), predator->worn.end(),
                        row.baseId) == predator->worn.end());
      rows += row.count;
    }
    REQUIRE(rows == total - worn);
  }

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

  // thuum docs/verbs/npc-fate.md (playtest 44's 1c, the longbow): a take of
  // what the body's record never held is refused by name
  // (E_TAKE_NOT_IN_RECORD), no throw, the looter's engine resynced (its
  // own inventory and the body's), nothing moves
  constexpr uint32_t kLongBow = 0x3b562; // Skyrim.esm LongBow
  const auto corpseBows = actor->GetInventory().GetItemCount(kLongBow);
  const auto looterBows = looter.GetInventory().GetItemCount(kLongBow);
  Inventory::Entry bow;
  bow.baseId = kLongBow;
  bow.count = corpseBows + 1;
  p.Messages().clear();
  REQUIRE_NOTHROW(actor->TakeItem(looter, bow));
  REQUIRE(looter.GetInventory().GetItemCount(kLongBow) == looterBows);
  REQUIRE(actor->GetInventory().GetItemCount(kLongBow) == corpseBows);
  REQUIRE(SentOfType(p, 0, MsgType::SetInventory) >= 1);
  {
    // the body's record too (a corpse is a container to its looter)
    bool bodySent = false;
    for (const auto& msg : SentTo<UpdatePropertyMessage>(p, 0)) {
      if (msg.idx == actor->GetIdx() && msg.propName == "inventory") {
        bodySent = true;
      }
    }
    REQUIRE(bodySent);
  }

  actor->Respawn(false);
  p.worldState.fateRespawnDue.erase(kPredator);
  DoDisconnect(p, 0);
  p.DestroyActor(kLooter);
}

// thuum docs/verbs/npc-fate.md (playtest 48, the loot strip; H2 of its
// read): a take from a body removed the record's row and left the worn entry,
// so the body's fate kept naming the taken piece and a seat's reconcile added
// it back. The take now drops the worn entry it empties, an unworn copy
// leaving first, and the fate names a worn piece only while the inventory
// holds it (a record saved before the drop keeps such an entry). The predator
// stands for a looted bandit with a roll of its own: the hide boots worn, two
// hide helmets with one worn (Skyrim.esm ArmorHideBoots and ArmorHideHelmet,
// the hosted swing case's ids)
TEST_CASE("A take from a body drops the record's worn entry it empties, an "
          "unworn copy leaving first, and the body's fate names no worn piece "
          "its inventory lacks",
          "[NpcFate][espm]")
{
  enum : uint32_t
  {
    kLooter = 0xff000d42,
    kLater = 0xff000d43,
    kHideBoots = 0x13910,
    kHideHelmet = 0x13913
  };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
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

  // the body opens to the looter, its occupant, so the takes are the
  // looter's (ContainerMoveByOccupant)
  actor->Kill(nullptr);
  REQUIRE(actor->IsDead());
  actor->Activate(looter);
  REQUIRE(actor->IsContainerOccupant(&looter));

  // the roll, laid after the open's own (EnsureBaseContainerAdded); a fresh
  // actor is never assumed empty, so the counts are read back
  actor->AddItem(kHideBoots, 1);
  actor->AddItem(kHideHelmet, 2);
  Equipment rolled;
  rolled.numChanges = 7;
  rolled.inv.entries.push_back(Inventory::Entry(kHideBoots, 1, kWornRight));
  rolled.inv.entries.push_back(Inventory::Entry(kHideHelmet, 1, kWornRight));
  actor->SetEquipment(rolled);
  const auto boots = actor->GetInventory().GetItemCount(kHideBoots);
  const auto helmets = actor->GetInventory().GetItemCount(kHideHelmet);
  REQUIRE(boots >= 1);
  REQUIRE(helmets >= 2);
  const auto take = [&](uint32_t baseId, uint32_t count) {
    Inventory::Entry entry;
    entry.baseId = baseId;
    entry.count = count;
    actor->TakeItem(looter, entry);
  };

  // a helmet of the two: the record still holds one, the worn one stays
  take(kHideHelmet, 1);
  REQUIRE(actor->GetInventory().GetItemCount(kHideHelmet) == helmets - 1);
  REQUIRE(Wearing(*actor, kHideHelmet));
  REQUIRE(Wearing(*actor, kHideBoots));

  // the worn boots: their worn entry leaves with them
  take(kHideBoots, boots);
  REQUIRE(actor->GetInventory().GetItemCount(kHideBoots) == 0);
  REQUIRE_FALSE(Wearing(*actor, kHideBoots));
  REQUIRE(Wearing(*actor, kHideHelmet));

  // the helmets left: the worn one goes too
  take(kHideHelmet, helmets - 1);
  REQUIRE(actor->GetInventory().GetItemCount(kHideHelmet) == 0);
  REQUIRE_FALSE(Wearing(*actor, kHideHelmet));
  REQUIRE(looter.GetInventory().GetItemCount(kHideHelmet) >= helmets);
  // the counter stays: a copy's view re-applies a changed one whole
  REQUIRE(actor->GetEquipment().numChanges == 7);

  // a record saved before the take's drop: the boots worn, none held. A
  // later player's arrival hears the body's fate, which names no boots
  Equipment stale = actor->GetEquipment();
  stale.inv.entries.push_back(Inventory::Entry(kHideBoots, 1, kWornRight));
  actor->SetEquipment(stale);
  REQUIRE(Wearing(*actor, kHideBoots));
  DoConnect(p, 1);
  p.CreateActor(kLater, kForest, 0, kTamriel);
  p.SetUserActor(1, kLater);
  p.Messages().clear();
  p.TickCellHosts(FateNowMs());
  const auto fates = SentTo<ActorFateMessage>(p, 1);
  const ActorFateMessage::Fate* sent = nullptr;
  for (const auto& msg : fates) {
    for (const auto& f : msg.fates) {
      if (f.refrId == kPredator) {
        sent = &f;
      }
    }
  }
  REQUIRE(sent != nullptr);
  REQUIRE(std::find(sent->worn.begin(), sent->worn.end(), kHideBoots) ==
          sent->worn.end());
  REQUIRE(std::find(sent->worn.begin(), sent->worn.end(), kHideHelmet) ==
          sent->worn.end());

  // the shared world: the body's equipment as found, the predator standing
  actor->SetEquipment(Equipment{});
  actor->Respawn(false);
  p.worldState.fateRespawnDue.erase(kPredator);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kLooter);
  p.DestroyActor(kLater);
}

// thuum docs/verbs/npc-fate.md (playtest 37, findings 58 and 62): a
// server-made NPC's body is a corpse to the server as a game-file actor's
// is. IsCorpse read the id's range, so rotfern's E on the dead necromancer
// ff00000a (a 0xff copy) matched no branch of ProcessActivateNormal and was
// dropped with no line. The looter need not host the body's cell, and the
// loot stands once the host is gone: the open answers the activator alone
// with the record's inventory and OpenContainer, the closing activation
// releases the occupancy, and a live copy opens nothing.
TEST_CASE("A dead server-made NPC is a corpse to any player beside it, its "
          "host or not",
          "[NpcFate][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kHost = 0xff000d32;
  constexpr uint32_t kLooter = 0xff000d33;
  constexpr uint32_t kMade = 0xff000d34;
  constexpr uint32_t kBanditBase = 0x3de6f; // EncBandit01Melee2HBerserkNordM
  constexpr uint32_t kGold = 0xf;

  // the host takes the forest's cell; the looter arrives beside it
  DoConnect(p, 0);
  p.CreateActor(kHost, kForest, 0, kTamriel);
  p.SetUserActor(0, kHost);
  HostGrants(p, 0, kForestCell);
  DoConnect(p, 1);
  p.CreateActor(kLooter, kForest, 0, kTamriel);
  p.SetUserActor(1, kLooter);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& looter = p.worldState.GetFormAt<MpActor>(kLooter);

  // the server-made bandit (the placed copy's idiom above) stands at the
  // forest, hosted by user 0
  MpChangeForm saved;
  saved.recType = MpChangeForm::ACHR;
  saved.formDesc = FormDesc::FromFormId(kMade, p.worldState.espmFiles);
  saved.baseDesc = FormDesc::FromFormId(kBanditBase, p.worldState.espmFiles);
  saved.worldOrCellDesc = FormDesc::Tamriel();
  saved.position = kForest;
  saved.angle = { 0.f, 0.f, 0.f };
  p.worldState.LoadChangeForm(saved, p.CreateFormCallbacks());
  p.worldState.LoadDeferredActors();
  auto& form = p.worldState.LookupFormByIdNoLoad(kMade);
  REQUIRE(form);
  MpActor* made = form->AsActor();
  REQUIRE(made);
  REQUIRE_FALSE(made->IsCreatedAsPlayer());
  p.worldState.fateRespawnDue.erase(kMade);
  looter.SetPos(made->GetPos() + NiPoint3{ 60.f, 0.f, 0.f });
  REQUIRE(p.worldState.HostOf(*made) == &host);

  const auto activate = [&](Networking::UserId user, bool second) {
    DoMessage(p, user,
              nlohmann::json{ { "t", MsgType::Activate },
                              { "data",
                                { { "caster", 0x14 },
                                  { "target", kMade },
                                  { "isSecondActivation", second } } } });
  };
  const auto inventorySentTo = [&](Networking::UserId user) {
    for (const auto& msg : SentTo<UpdatePropertyMessage>(p, user)) {
      if (msg.idx == made->GetIdx() && msg.propName == "inventory") {
        return true;
      }
    }
    return false;
  };

  // alive: no corpse, nothing opens on the server
  REQUIRE_FALSE(made->IsCorpse());
  p.Messages().clear();
  activate(1, false);
  REQUIRE_FALSE(made->IsOpen());
  REQUIRE(SentTo<OpenContainerMessage>(p, 1).empty());

  // dead: a corpse, the CONT branch's open for the looter, who does not
  // host it; the host hears no OpenContainer
  made->Kill(nullptr);
  REQUIRE(made->IsDead());
  REQUIRE(made->IsCorpse());
  made->AddItem(kGold, 3);
  // the shared world: neither inventory starts empty, so the take is read
  // as a difference
  const auto corpseGold = made->GetInventory().GetItemCount(kGold);
  const auto looterGold = looter.GetInventory().GetItemCount(kGold);
  p.Messages().clear();
  activate(1, false);
  REQUIRE(made->IsOpen());
  REQUIRE(inventorySentTo(1));
  REQUIRE(SentTo<OpenContainerMessage>(p, 1).size() == 1);
  REQUIRE(SentTo<OpenContainerMessage>(p, 0).empty());
  Inventory::Entry gold;
  gold.baseId = kGold;
  gold.count = 3;
  made->TakeItem(looter, gold);
  REQUIRE(looter.GetInventory().GetItemCount(kGold) == looterGold + 3);
  REQUIRE(made->GetInventory().GetItemCount(kGold) == corpseGold - 3);

  // the closing activation releases the occupancy
  activate(1, true);
  REQUIRE_FALSE(made->IsOpen());

  // the host gone, the body opens again: the loot depends on no host
  DoDisconnect(p, 0);
  p.DestroyActor(kHost);
  p.Messages().clear();
  activate(1, false);
  REQUIRE(made->IsOpen());
  REQUIRE(inventorySentTo(1));
  REQUIRE(SentTo<OpenContainerMessage>(p, 1).size() == 1);
  activate(1, true);
  REQUIRE_FALSE(made->IsOpen());

  // the shared world
  made->Respawn(false);
  p.worldState.fateRespawnDue.erase(kMade);
  DoDisconnect(p, 1);
  p.DestroyActor(kMade);
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

// thuum docs/verbs/npc-fate.md (playtest 37, 2026-10-28, findings 43 and
// 46): the server's hit gate judges a hosted actor's swing by the weapon in
// its record's hands (E_HIT_WEAPON), and the host's engine wields what its
// own inventory roll gave it when the fate reached it late (an interior's
// actor, a login's neighbours: the redress dresses the outfit from the
// server's list and leaves the engine's rolled weapon in place), so the
// hands report named a weapon the record's inventory lacked, the report was
// refused whole and every swing fell. The record's worn set is also the
// fate's worn list, and a report that lacked the outfit (a copy polled
// before its dressing) emptied it, so the next fate redressed the copy
// naked. The predator stands for a game-file actor with a rolled record:
// boots worn and an iron sword in hand.
TEST_CASE("A hosted game-file actor's swing lands with the weapon its "
          "record's hands hold, falls with one the record lacks until its "
          "host's equipment stream names it, and the stream's weapon is "
          "recorded beside the server's outfit, which a report lacking the "
          "outfit never shrinks",
          "[NpcFate][HostedEquipment][espm]")
{
  enum : uint32_t
  {
    kHost = 0xff000d30,
    kOther = 0xff000d31,
    // Skyrim.esm: IronSword, SteelWarAxe (the engine's roll on 39fe4,
    // playtest 37), ArmorHideBoots, ArmorHideHelmet
    kIronSword = 0x12eb7,
    kSteelWarAxe = 0x13983,
    kHideBoots = 0x13910,
    kHideHelmet = 0x13913
  };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  DoConnect(p, 0);
  p.CreateActor(kHost, kForest, 0, kTamriel);
  p.SetUserActor(0, kHost);
  DoConnect(p, 1);
  p.CreateActor(kOther, kForest + NiPoint3{ 200.f, 0.f, 0.f }, 0, kTamriel);
  p.SetUserActor(1, kOther);
  // the fate pass loads the ring; the predator stands beside the other
  // player, alive, with no deadline
  p.TickCellHosts(FateNowMs());
  auto& form = p.worldState.LookupFormByIdNoLoad(kPredator);
  REQUIRE(form);
  MpActor* predatorPtr = form->AsActor();
  REQUIRE(predatorPtr);
  MpActor& predator = *predatorPtr;
  if (predator.IsDead()) {
    predator.Respawn(false);
  }
  p.worldState.fateRespawnDue.erase(kPredator);
  predator.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  HostGrants(p, 0, kForestCell);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  REQUIRE(p.worldState.HostOf(predator) == &host);
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(other.SetActorValueRecord(record));

  // the record's roll: boots worn, the sword in hand, both in its inventory
  predator.AddItem(kIronSword, 1);
  predator.AddItem(kHideBoots, 1);
  Equipment rolled;
  rolled.inv.entries.push_back(Inventory::Entry(kHideBoots, 1, kWornRight));
  rolled.inv.entries.push_back(Inventory::Entry(kIronSword, 1, kWornRight));
  predator.SetEquipment(rolled);
  p.Messages().clear();

  // a swing with the record's sword lands
  const float whole = HealthOf(p, kOther);
  Swings(p, predator, kOther, kIronSword);
  const float afterSword = HealthOf(p, kOther);
  REQUIRE(afterSword < whole);

  // with the engine's own war axe, which the record never rolled and the
  // stream has not named: dropped (E_HIT_WEAPON)
  Swings(p, predator, kOther, kSteelWarAxe);
  REQUIRE(HealthOf(p, kOther) == afterSword);

  // the host's stream names the axe: recorded (R2), the item in the record's
  // inventory, the sword out of the hands, the boots the report lacked kept;
  // the report reaches the other game and nothing comes back to the host
  p.Messages().clear();
  Wears(p, predator, { kSteelWarAxe }, 1);
  REQUIRE(predator.GetInventory().HasItem(kSteelWarAxe));
  REQUIRE(Wearing(predator, kSteelWarAxe));
  REQUIRE_FALSE(Wearing(predator, kIronSword));
  REQUIRE(Wearing(predator, kHideBoots));
  REQUIRE(predator.GetEquipment().numChanges == 1);
  REQUIRE(SentOfType(p, 1, MsgType::UpdateEquipment) == 1);
  REQUIRE(SentOfType(p, 0, MsgType::UpdateEquipment) == 0);
  REQUIRE(SentOfType(p, 0, MsgType::SetInventory) == 0);

  // the swing with the axe lands now
  Swings(p, predator, kOther, kSteelWarAxe);
  REQUIRE(HealthOf(p, kOther) < afterSword);

  // a report with nothing worn (the copy polled before its dressing): the
  // hands follow it, the outfit stays the server's
  Wears(p, predator, {}, 2);
  REQUIRE_FALSE(Wearing(predator, kSteelWarAxe));
  REQUIRE(Wearing(predator, kHideBoots));
  REQUIRE(predator.GetEquipment().numChanges == 2);

  // a worn item that is no weapon and that the record lacks refuses the
  // report as before
  Wears(p, predator, { kHideHelmet }, 3);
  REQUIRE_FALSE(predator.GetInventory().HasItem(kHideHelmet));
  REQUIRE_FALSE(Wearing(predator, kHideHelmet));
  REQUIRE(Wearing(predator, kHideBoots));
  REQUIRE(predator.GetEquipment().numChanges == 2);

  // the shared world: the predator's hands and inventory as found
  predator.SetEquipment(Equipment{});
  predator.RemoveItem(kIronSword, 1, nullptr);
  predator.RemoveItem(kHideBoots, 1, nullptr);
  predator.RemoveItem(kSteelWarAxe, 1, nullptr);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHost);
  p.DestroyActor(kOther);
}

// thuum docs/verbs/npc-fate.md ("Reanimation is any player's, on any body";
// Eli, playtest 37: "anyone should be able to reanimate any corpse, that's
// Just Gameplay"; ADR-060, playtest 41, Eli: "and yes go with A"): a
// reanimate's accepted hit hands the body to the caster's seat for the
// effect's life (the cells rule's commanded claim, UpdateHoster to every
// game); the caster's own engine ran the spell and runs the effect, so the
// get-up, the shader and the commanded packages play where the commander
// is. The former host hears the relayed cast as any cast's look; the
// snippet that cast the spell again on the host's engine (playtest 41: the
// effect ran there with a figure as the commander, "puts him back on his
// normal path") is gone. Skyrim.esm: Reanimate Corpse 0x65bd7, its effect
// ReanimateFFAimed25 0x96d0b of the Reanimate archetype (libespm
// MGEF::EffectType::Reanimate, 22, as CommonLibSSE-NG's EffectArchetypes.h
// kReanimate), and Dead Thrall 0x7e8df, its effect ReanimateThrallFFAimed
// 0x7e8e0, read here from the records themselves. Dead Thrall's full
// thrall sections are the next case's, on a humanoid body: 0x7e8e0's
// conditions want ActorTypeNPC, which the predator lacks (pipeline 5939).
TEST_CASE("A reanimate's accepted hit hands the body to the caster's seat "
          "for the effect's life; its death ends the claim; the host's own "
          "cast keeps the body its own",
          "[NpcFate][Reanimate][espm]")
{
  constexpr uint32_t kHost = 0xff000d35;
  constexpr uint32_t kCaster = 0xff000d36;
  constexpr uint32_t kReanimateCorpse = 0x65bd7;
  constexpr uint32_t kReanimateEffect = 0x96d0b;
  constexpr uint32_t kDeadThrall = 0x7e8df;
  constexpr uint32_t kDeadThrallEffect = 0x7e8e0;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;

  // the record itself: `spell`'s Reanimate effect is `effectId`
  const auto reanimatesBy = [&](uint32_t spellId, uint32_t effectId) {
    const auto spell = espm::GetData<espm::SPEL>(spellId, &p.worldState);
    REQUIRE(spell.spellItem != nullptr);
    bool reanimates = false;
    for (const auto& effect : MpActor::EffectsOf(&p.worldState, spellId)) {
      const auto mgef =
        espm::GetData<espm::MGEF>(effect.effectId, &p.worldState);
      if (mgef.data.effectType == espm::MGEF::EffectType::Reanimate) {
        REQUIRE(effect.effectId == effectId);
        reanimates = true;
      }
    }
    REQUIRE(reanimates);
  };
  reanimatesBy(kReanimateCorpse, kReanimateEffect);
  reanimatesBy(kDeadThrall, kDeadThrallEffect);

  // the host takes the forest's cell; the caster arrives beside the body,
  // which lies dead, hosted by user 0
  DoConnect(p, 0);
  p.CreateActor(kHost, kForest, 0, kTamriel);
  p.SetUserActor(0, kHost);
  DoConnect(p, 1);
  p.CreateActor(kCaster, kForest, 0, kTamriel);
  p.SetUserActor(1, kCaster);
  p.TickCellHosts(FateNowMs());
  auto& form = p.worldState.LookupFormByIdNoLoad(kPredator);
  REQUIRE(form);
  MpActor* bodyPtr = form->AsActor();
  REQUIRE(bodyPtr);
  MpActor& body = *bodyPtr;
  if (body.IsDead()) {
    body.Respawn(false);
  }
  p.worldState.fateRespawnDue.erase(kPredator);
  body.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  HostGrants(p, 0, kForestCell);
  // the caster's game has the cell loaded too, as its client reports it: a
  // command needs its commander to have the body's cell loaded (cells.rs
  // claim_commanded answers Unloaded otherwise, E_COMMAND_UNLOADED); the
  // host holds the cell, so the load grants nothing
  {
    RawMessageData raw;
    raw.userId = 1;
    CellLoadedMessage loaded;
    loaded.cell = kForestCell;
    loaded.loaded = true;
    p.GetActionListener().OnCellLoaded(raw, loaded);
  }
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  REQUIRE(p.worldState.HostOf(body) == &host);
  body.Kill(nullptr);
  REQUIRE(body.IsDead());

  // `user`'s cast of `spell` (Reanimate Corpse or Dead Thrall) through the
  // packet path, as its client's message arrives: the caster 0x14, the
  // target the body (a client naming the crosshair's actor; the engine's own
  // word for an aimed cast names the caster, and then the relay stands and
  // the look doubles on the host once, RelayCast)
  const auto casts = [&](Networking::UserId user, uint32_t spell) {
    DoMessage(p, user,
              nlohmann::json{
                { "t", static_cast<int>(MsgType::SpellCast) },
                { "data",
                  { { "caster", 0x14 },
                    { "target", kPredator },
                    { "spell", spell },
                    { "isDualCasting", false },
                    { "interruptCast", false },
                    { "castingSource", 1 },
                    { "aimAngle", 0.f },
                    { "aimHeading", 0.f },
                    { "hasLaunch", false },
                    { "originX", 0.f },
                    { "originY", 0.f },
                    { "originZ", 0.f },
                    { "launchAngleX", 0.f },
                    { "launchAngleZ", 0.f },
                    { "actorAnimationVariables",
                      { { "booleans", nlohmann::json::array() },
                        { "floats", nlohmann::json::array() },
                        { "integers", nlohmann::json::array() } } } } } });
  };
  // `user`'s report of its own hit on the body with `spell`
  const auto hits = [&](Networking::UserId user, uint32_t spell) {
    RawMessageData raw;
    raw.userId = user;
    HitMessage hit;
    hit.data.aggressor = 0x14;
    hit.data.target = kPredator;
    hit.data.source = spell;
    p.GetActionListener().OnHit(raw, hit);
  };
  // the Spell.Cast snippets sent to `user` since the last clear, known by
  // class and function (CellHostTest's idiom)
  const auto castSnippetsTo = [&](Networking::UserId user) {
    std::vector<nlohmann::json> out;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::SpSnippet) &&
          m.j.contains("class") && m.j["class"] == "Spell" &&
          m.j.contains("function") && m.j["function"] == "Cast") {
        out.push_back(m.j);
      }
    }
    return out;
  };

  // UpdateProperty isHostedByOther for the body sent to `user` since the
  // last clear (UpdateHoster's word)
  const auto hosterWordsTo = [&](Networking::UserId user) {
    size_t count = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::UpdateProperty) &&
          m.j.contains("propName") && m.j["propName"] == "isHostedByOther" &&
          m.j.contains("idx") && m.j["idx"] == body.GetIdx()) {
        ++count;
      }
    }
    return count;
  };

  // the same word saying this game simulates it (isHostedByOther false)
  const auto hostedFalseTo = [&](Networking::UserId user) {
    size_t count = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::UpdateProperty) &&
          m.j.contains("propName") && m.j["propName"] == "isHostedByOther" &&
          m.j.contains("idx") && m.j["idx"] == body.GetIdx() &&
          m.j.value("dataDump", std::string()) == "false") {
        ++count;
      }
    }
    return count;
  };

  // the MagicEffects words for the body sent to `user` since the last clear
  // (MagicEffectsTest's idiom); a listener NPC the host hosts doubles the
  // host's, so a test reads every word, not their count
  const auto effectWordsTo = [&](Networking::UserId user) {
    std::vector<nlohmann::json> out;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::MagicEffects) &&
          m.j.contains("idx") && m.j["idx"] == body.GetIdx()) {
        out.push_back(m.j);
      }
    }
    return out;
  };

  // the record's own duration of `spell`'s Reanimate effect, seconds: the
  // command's side at its commander's departure, a day and more a full
  // thrall's (wire-rules magic::FULL_THRALL_MIN_S, 86,400 s), less a timed
  // one's; never a lease or a clock
  const auto recordSecondsOf = [&](uint32_t spell, uint32_t effectId) {
    uint32_t seconds = 0;
    for (const auto& effect : MpActor::EffectsOf(&p.worldState, spell)) {
      if (effect.effectId == effectId) {
        seconds = effect.duration;
      }
    }
    return seconds;
  };

  // the commander's engine stands the thrall: its live sample of the body,
  // the death past the grace and the reanimate's hit inside its window
  // (OnHostedActorState's resurrection branch, PartOne::ResurrectByHost),
  // the one respawn a command survives
  const auto standsOn = [&](Networking::UserId user) {
    body.RecordDeathAt(
      std::chrono::steady_clock::now() -
      std::chrono::milliseconds(MpActor::kPendingDeathMs + 1000));
    State(p, user, kPredator, 1.f);
  };

  // the items the body carries, every entry's count: a death item's roll
  // adds one at least, whatever species the predator rolled (each of
  // LCharAnimalForestPredator 0x42297's has a death item of chance none 0
  // and Use All holding a base item: a tail, a pelt, venom or fat;
  // Skyrim.esm)
  const auto items = [&] {
    uint32_t n = 0;
    for (const auto& entry : body.GetInventory().entries) {
      n += entry.count;
    }
    return n;
  };

  SECTION("by a player who is not the host: the body is the caster's seat's "
          "from the hit, both games hear the word, no snippet, the relay "
          "reaches the host as any cast's; the body's death ends the claim")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    p.Messages().clear();
    casts(1, kReanimateCorpse);
    // the relay reaches the host as any cast's look (the skip is gone)
    REQUIRE(SentOfType(p, 0, MsgType::SpellCast) == 1);
    hits(1, kReanimateCorpse);
    p.Tick(); // snippets would be deferred (SpSnippet.cpp): none come
    REQUIRE(castSnippetsTo(0).empty());
    REQUIRE(castSnippetsTo(1).empty());
    // the caster's seat simulates the body from the hit (ADR-060)
    REQUIRE(p.SimulatorOf(body) == kCaster);
    REQUIRE(p.worldState.HostOf(body) == &caster);
    REQUIRE(hosterWordsTo(0) >= 1);
    REQUIRE(hosterWordsTo(1) >= 1);
    // a second hit renews the command, nothing else
    hits(1, kReanimateCorpse);
    REQUIRE(p.SimulatorOf(body) == kCaster);
    // the commander's engine stands the thrall: the command stands through
    // it
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == kCaster);
    REQUIRE(p.SimulatorOf(body) == kCaster);
    // the thrall's death (the effect's end in the commander's engine, its
    // report) ends the claim: the cell's host simulates the body again
    p.Messages().clear();
    body.Kill(nullptr);
    REQUIRE(body.IsDead());
    REQUIRE(p.SimulatorOf(body) == kHost);
    REQUIRE(p.worldState.HostOf(body) == &host);
    REQUIRE(hosterWordsTo(0) >= 1);
    REQUIRE(hosterWordsTo(1) >= 1);
  }

  SECTION("by a player who is not the host: the thrall's ash after its death "
          "is its reanimate's caster's for wire-rules natives' "
          "ASH_WINDOW_MS, relayed to the host, and refused past it (the "
          "reanimate-fx memo's D3 B)")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    casts(1, kReanimateCorpse);
    hits(1, kReanimateCorpse);
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(body.ReanimateAcceptedCaster() == kCaster);
    // the effect's end in the commander's engine: the command ends at the
    // death, the cell's host simulates the body again
    body.Kill(nullptr);
    REQUIRE(body.IsDead());
    REQUIRE(p.SimulatorOf(body) == kHost);

    // vanilla ReanimateAshPile's TurnToAsh on the commander's engine (MGEF
    // 0xf52ab), as its seat reports it: AttachAshPile(None) on the body and
    // ShockDisintegrate01FXS 0xd22fb's Stop on it
    using Arg = std::optional<
      std::variant<bool, double, std::string, SpSnippetObjectArgument>>;
    SpSnippetObjectArgument none;
    none.formId = 0;
    none.type = "Form";
    SpSnippetObjectArgument victim;
    victim.formId = kPredator;
    victim.type = "Actor";
    const auto ashCall = [&](uint32_t selfId, const char* cls, const char* fn,
                             std::vector<Arg> args) {
      ScriptCallMessage msg;
      msg.selfId = selfId;
      msg.cls = cls;
      msg.fn = fn;
      msg.args = std::move(args);
      msg.caller = "ReanimateAshPile";
      msg.handler = "TurnToAsh";
      RawMessageData raw;
      raw.userId = 1;
      p.GetActionListener().OnScriptCall(raw, msg);
    };
    // the relays of `fn` sent to `user` since the last clear
    const auto relaysTo = [&](Networking::UserId user, const char* fn) {
      size_t count = 0;
      for (auto& m : p.Messages()) {
        if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
            m.j["t"] == static_cast<int>(MsgType::SpSnippet) &&
            m.j.contains("function") && m.j["function"] == fn) {
          ++count;
        }
      }
      return count;
    };

    // inside the window: the caster's calls go to the body's other
    // listener, the host, never back to the caster
    p.Messages().clear();
    ashCall(kPredator, "Actor", "AttachAshPile", { Arg(none) });
    ashCall(0xd22fb, "EffectShader", "Stop", { Arg(victim) });
    REQUIRE(relaysTo(0, "AttachAshPile") == 1);
    REQUIRE(relaysTo(0, "Stop") == 1);
    REQUIRE(relaysTo(1, "AttachAshPile") == 0);

    // past it: the simulator's refusal, nothing relayed
    body.RecordDeathAt(
      std::chrono::steady_clock::now() -
      std::chrono::milliseconds(
        static_cast<int64_t>(skymp::rules::script_ash_window_ms()) + 1000));
    p.Messages().clear();
    {
      LogCapture log;
      ashCall(kPredator, "Actor", "AttachAshPile", { Arg(none) });
      REQUIRE(log.Count("E_SCRIPTCALL_OWNER") == 1);
      REQUIRE(log.Count("past the ash window") == 1);
    }
    REQUIRE(relaysTo(0, "AttachAshPile") == 0);
  }

  SECTION("by a player who is not the host: the stand-up's MagicEffects "
          "names the Reanimate effect with no end (-1) to both games, and the "
          "thrall's death sends both the emptied set (its look, ADR-060)")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    casts(1, kReanimateCorpse);
    hits(1, kReanimateCorpse);
    REQUIRE(p.SimulatorOf(body) == kCaster);
    // a dead target takes nothing at the hit
    REQUIRE_FALSE(body.HasRunningEffects());
    // the commander's engine stands it (standsOn: its live sample past the
    // death's grace, inside the hit's window); the words read below are the
    // stand-up's
    p.Messages().clear();
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    // a state with no clock, sent as -1, no end (thuum docs/WIRE.md,
    // MagicEffects' remaining; the platform review of 3a31c05a, F5), while
    // the record keeps its 60 s (Skyrim.esm's EFIT) below; the Dark Souls
    // rider fails its HasPerk on the caster (EffectConditionsTest) and the
    // ash's script effect is no state of the server's, so the set is the one
    // effect
    for (const Networking::UserId user : { 0, 1 }) {
      auto words = effectWordsTo(user);
      REQUIRE_FALSE(words.empty());
      for (auto& word : words) {
        REQUIRE(word["effects"].size() == 1);
        REQUIRE(word["effects"][0]["effect"] == kReanimateEffect);
        REQUIRE(word["effects"][0]["source"] == kReanimateCorpse);
        REQUIRE(word["effects"][0]["remaining"].get<float>() == -1.f);
      }
    }
    const auto running = body.GetRunningEffects();
    REQUIRE(running.size() == 1);
    REQUIRE(running[0].effect == kReanimateEffect);
    REQUIRE(running[0].secondsLeft ==
            Catch::Approx(static_cast<float>(
              recordSecondsOf(kReanimateCorpse, kReanimateEffect))));
    // the commander's engine is its timer (ADR-060 as amended): past the
    // record's 60 s and past a day the state runs on and no word drops it,
    // so no game dispels a living thrall
    p.Messages().clear();
    body.AdvanceEffects(61.f);
    body.AdvanceEffects(86'400.f);
    REQUIRE(body.GetRunningEffects().size() == 1);
    REQUIRE(effectWordsTo(0).empty());
    REQUIRE(effectWordsTo(1).empty());
    // its death: the effects tick that reads it ends the set and tells both
    // games, so each stops the shader at the death
    p.Messages().clear();
    body.Kill(nullptr);
    REQUIRE(body.IsDead());
    body.AdvanceEffects(0.25f);
    REQUIRE_FALSE(body.HasRunningEffects());
    for (const Networking::UserId user : { 0, 1 }) {
      auto words = effectWordsTo(user);
      REQUIRE_FALSE(words.empty());
      for (auto& word : words) {
        REQUIRE(word["effects"].empty());
      }
    }
  }

  SECTION("by a player who is not the host: no lease ends the command at "
          "the record's duration, and its commander's report of the thrall's "
          "death ends it at any time, past the reanimation's 90 s window")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    casts(1, kReanimateCorpse);
    hits(1, kReanimateCorpse);
    REQUIRE(p.SimulatorOf(body) == kCaster);
    REQUIRE(p.CommanderOf(body) == kCaster);
    // Reanimate Corpse's record says 60 s (the old lease, 70 s with its
    // margin): the clock past it ends nothing (Eli, 2026-11-12: "summons
    // and resurrections should obey the timers the level of spell")
    p.TickCellHosts(FateNowMs() + 71'000);
    REQUIRE(p.SimulatorOf(body) == kCaster);
    REQUIRE(p.CommanderOf(body) == kCaster);
    // the commander's engine stood the thrall long ago (a dual cast's or a
    // perk's longer effect) and now ends it: the hit and the stand-up older
    // than every other evidence's window, the record alive
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == kCaster);
    const auto now = std::chrono::steady_clock::now();
    body.NoteHitBy(kCaster, now - std::chrono::seconds(130));
    body.NoteHostStoodUp(now -
                         std::chrono::milliseconds(MpActor::kReanimatedEndMs) -
                         std::chrono::seconds(30));
    const bool reportsBefore = p.worldState.npcDeathReports;
    p.worldState.npcDeathReports = true;
    {
      ActorDeathMessage report;
      report.refrId = kPredator;
      report.cause = 0;
      report.killer = 0;
      const auto& at = body.GetPos();
      report.pos = { at.x, at.y, at.z };
      RawMessageData raw;
      raw.userId = 1;
      p.GetActionListener().OnActorDeath(raw, report);
    }
    p.worldState.npcDeathReports = reportsBefore;
    REQUIRE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
  }

  SECTION("by a player who is not the host: the thrall's death at its "
          "commander's report rolls no death item, its body keeping what it "
          "carried; respawned, the NPC's next death rolls it (single "
          "player's rule, MpActor's StandsByReanimation)")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    casts(1, kReanimateCorpse);
    hits(1, kReanimateCorpse);
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    // the stand-up's reanimate state on the record, the server's own fact
    const auto running = body.GetChangeForm().runningEffects;
    REQUIRE(running.has_value());
    REQUIRE(std::any_of(running->begin(), running->end(),
                        [](const RunningEffect& r) { return r.reanimate; }));
    const uint32_t carried = items();
    const auto now = std::chrono::steady_clock::now();
    body.NoteHitBy(kCaster, now - std::chrono::seconds(130));
    body.NoteHostStoodUp(now -
                         std::chrono::milliseconds(MpActor::kReanimatedEndMs) -
                         std::chrono::seconds(30));
    const bool reportsBefore = p.worldState.npcDeathReports;
    p.worldState.npcDeathReports = true;
    {
      LogCapture log;
      ActorDeathMessage report;
      report.refrId = kPredator;
      report.cause = 0;
      report.killer = 0;
      const auto& at = body.GetPos();
      report.pos = { at.x, at.y, at.z };
      RawMessageData raw;
      raw.userId = 1;
      p.GetActionListener().OnActorDeath(raw, report);
      REQUIRE(log.Count("dies from its reanimation: no death item rolled") ==
              1);
    }
    p.worldState.npcDeathReports = reportsBefore;
    REQUIRE(body.IsDead());
    REQUIRE(items() == carried);
    // the death's effects tick ends the state; a respawn stands the NPC
    // alive with none, and its next death rolls the death item
    body.AdvanceEffects(0.25f);
    REQUIRE_FALSE(body.HasRunningEffects());
    body.Respawn(false);
    REQUIRE_FALSE(body.IsDead());
    const uint32_t standing = items();
    body.Kill(nullptr);
    REQUIRE(body.IsDead());
    REQUIRE(items() > standing);
  }

  SECTION("by a player who is not the host: the command outlives its "
          "commander's unload of the thrall's cell, with no word (Eli, "
          "2026-11-12: \"leaving a cell should not kill the thrall\")")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    casts(1, kReanimateCorpse);
    hits(1, kReanimateCorpse);
    REQUIRE(p.CommanderOf(body) == kCaster);
    p.Messages().clear();
    {
      RawMessageData raw;
      raw.userId = 1;
      CellLoadedMessage unloaded;
      unloaded.cell = kForestCell;
      unloaded.loaded = false;
      p.GetActionListener().OnCellLoaded(raw, unloaded);
    }
    REQUIRE(p.CommanderOf(body) == kCaster);
    REQUIRE(p.SimulatorOf(body) == kCaster);
    REQUIRE(p.worldState.HostOf(body) == &caster);
    REQUIRE(hosterWordsTo(0) == 0);
    REQUIRE(hosterWordsTo(1) == 0);
  }

  SECTION("by a player who is not the host: the commander's disconnect kills "
          "a timed thrall at once by the server's word, then hands the body "
          "to the cell's host with the word, no clock kept (Eli, "
          "2026-11-12: \"'but as a plain resident with no undead look' "
          "should simply die\")")
  {
    // Reanimate Corpse's record: a timed thrall's, under a day
    REQUIRE(recordSecondsOf(kReanimateCorpse, kReanimateEffect) > 0);
    REQUIRE(
      static_cast<float>(recordSecondsOf(kReanimateCorpse, kReanimateEffect)) <
      skymp::rules::full_thrall_min_s());
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    casts(1, kReanimateCorpse);
    hits(1, kReanimateCorpse);
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == kCaster);
    const uint32_t carried = items();
    p.Messages().clear();
    DoDisconnect(p, 1);
    REQUIRE(body.IsDead());
    // the departure's kill rolls no death item (single player's rule)
    REQUIRE(items() == carried);
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
    REQUIRE(p.worldState.HostOf(body) == &host);
    // the kill's word reaches the host ahead of the hoster word, so the
    // host's engine promotes a body, never a living copy no effect holds
    size_t deadAt = p.Messages().size();
    size_t hostedAt = p.Messages().size();
    for (size_t i = 0; i < p.Messages().size(); ++i) {
      const auto& m = p.Messages()[i];
      if (m.userId != 0 || !m.j.is_object() ||
          m.j.value("t", -1) != static_cast<int>(MsgType::UpdateProperty) ||
          m.j.value("refrId", 0u) != kPredator) {
        continue;
      }
      const auto prop = m.j.value("propName", std::string());
      const auto dump = m.j.value("dataDump", std::string());
      if (prop == "isDead" && dump == "true") {
        deadAt = std::min(deadAt, i);
      }
      if (prop == "isHostedByOther" && dump == "false") {
        hostedAt = std::min(hostedAt, i);
      }
    }
    REQUIRE(deadAt < p.Messages().size());
    REQUIRE(hostedAt < p.Messages().size());
    REQUIRE(deadAt < hostedAt);
    // no estimate is kept: the clock past the record's duration changes
    // nothing
    p.TickCellHosts(FateNowMs() + 200'000);
    REQUIRE(body.IsDead());
    REQUIRE(p.SimulatorOf(body) == kHost);
    // the shared cleanup below disconnects the caster again
    DoConnect(p, 1);
    p.SetUserActor(1, kCaster);
  }

  SECTION("by a player who is not the host: a command its engine never stood "
          "ends at any other respawn, and the old commander's report of the "
          "living actor is a stranger's (E_DEATH_NOT_HOST)")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    casts(1, kReanimateCorpse);
    hits(1, kReanimateCorpse);
    REQUIRE(p.CommanderOf(body) == kCaster);
    REQUIRE(body.IsDead());
    // the fate's own stand-up (the sweep's RespawnFated; the console's
    // Resurrect and the lab's respawn-ref take the same path): no engine of
    // the commander's stood it, so the command ends
    body.RespawnFated();
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
    // the old commander's cause-0 report of it: refused, not the simulator
    const bool reportsBefore = p.worldState.npcDeathReports;
    p.worldState.npcDeathReports = true;
    {
      ActorDeathMessage report;
      report.refrId = kPredator;
      report.cause = 0;
      report.killer = 0;
      const auto& at = body.GetPos();
      report.pos = { at.x, at.y, at.z };
      RawMessageData raw;
      raw.userId = 1;
      p.GetActionListener().OnActorDeath(raw, report);
    }
    p.worldState.npcDeathReports = reportsBefore;
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.SimulatorOf(body) == kHost);
  }

  SECTION("by a player who is not the host: the commander's report of a body "
          "its engine never stood ends the command, the body dead here "
          "already")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    casts(1, kReanimateCorpse);
    hits(1, kReanimateCorpse);
    REQUIRE(p.CommanderOf(body) == kCaster);
    REQUIRE(body.IsDead());
    const bool reportsBefore = p.worldState.npcDeathReports;
    p.worldState.npcDeathReports = true;
    {
      ActorDeathMessage report;
      report.refrId = kPredator;
      report.cause = 0;
      report.killer = 0;
      const auto& at = body.GetPos();
      report.pos = { at.x, at.y, at.z };
      RawMessageData raw;
      raw.userId = 1;
      p.GetActionListener().OnActorDeath(raw, report);
    }
    p.worldState.npcDeathReports = reportsBefore;
    REQUIRE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
  }

  SECTION("by a player at another player's body: no command, no word")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    caster.SetEquipment(eq);
    host.Kill(nullptr);
    casts(1, kReanimateCorpse);
    p.Messages().clear();
    {
      LogCapture log;
      RawMessageData raw;
      raw.userId = 1;
      HitMessage hit;
      hit.data.aggressor = 0x14;
      hit.data.target = kHost;
      hit.data.source = kReanimateCorpse;
      p.GetActionListener().OnHit(raw, hit);
      REQUIRE(log.Count("a player's body is nobody's to command") == 1);
    }
    REQUIRE(p.CommanderOf(host) == 0);
    for (auto& m : p.Messages()) {
      REQUIRE_FALSE(
        (m.j.is_object() &&
         m.j.value("propName", std::string()) == "isHostedByOther" &&
         m.j.value("refrId", 0u) == kHost));
    }
    host.Respawn(false);
  }

  SECTION("by the host itself: no snippet, the body stays the host's, the "
          "relay reaches the other game as any cast's")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    host.SetEquipment(eq);
    p.Messages().clear();
    casts(0, kReanimateCorpse);
    REQUIRE(SentOfType(p, 1, MsgType::SpellCast) == 1);
    hits(0, kReanimateCorpse);
    p.Tick();
    REQUIRE(castSnippetsTo(0).empty());
    REQUIRE(castSnippetsTo(1).empty());
    REQUIRE(p.SimulatorOf(body) == kHost);
    REQUIRE(p.ReleaseAnyClaim(body) == kHost);
    REQUIRE(p.SimulatorOf(body) == kHost);
  }

  // thuum lab/.cache/drafts/reanimate/read-stage-125536.md 4.2
  // (x-reanimate-claim 20261112-125536's terminate): a commander that hosts
  // its thrall's cell is that cell's rule's answer through its own grace
  // (cells.rs gone()), so the departure's ClaimGone named the leaver and
  // sent it a keyframe its freed user could not take, in a ScopedTask's
  // destructor. The test's send target records what the live one throws on
  SECTION("by the host itself: its departure while it commands a timed "
          "thrall sends its own user nothing, the thrall dies by the "
          "server's word first, and the cell's next host takes the body")
  {
    Equipment eq;
    eq.rightSpell = kReanimateCorpse;
    host.SetEquipment(eq);
    casts(0, kReanimateCorpse);
    hits(0, kReanimateCorpse);
    REQUIRE(p.CommanderOf(body) == kHost);
    standsOn(0);
    REQUIRE_FALSE(body.IsDead());
    p.Messages().clear();
    {
      LogCapture log;
      REQUIRE_NOTHROW(DoDisconnect(p, 0));
      // the claim's end names nobody (PartOne.cpp's ClaimGone guard), and
      // every decision of the departure completed
      REQUIRE(log.Count("the claimant hosting the cell: the word waits for "
                        "its next host") == 1);
      REQUIRE(log.Count("a host decision threw") == 0);
    }
    REQUIRE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == 0);
    // nothing to the leaver's user: no keyframe and no hoster word
    const auto toLeaver =
      std::count_if(p.Messages().begin(), p.Messages().end(),
                    [](const PartOne::Message& m) { return m.userId == 0; });
    REQUIRE(toLeaver == 0);
    // the other game hears the death, the server's word (ThrallExpired),
    // ahead of the word that nobody simulates the body yet
    size_t deadAt = p.Messages().size();
    size_t hostedAt = p.Messages().size();
    for (size_t i = 0; i < p.Messages().size(); ++i) {
      const auto& m = p.Messages()[i];
      if (m.userId != 1 || !m.j.is_object() ||
          m.j.value("t", -1) != static_cast<int>(MsgType::UpdateProperty) ||
          m.j.value("refrId", 0u) != kPredator) {
        continue;
      }
      const auto prop = m.j.value("propName", std::string());
      const auto dump = m.j.value("dataDump", std::string());
      if (prop == "isDead" && dump == "true") {
        deadAt = std::min(deadAt, i);
      }
      if (prop == "isHostedByOther" && dump == "true") {
        hostedAt = std::min(hostedAt, i);
      }
    }
    REQUIRE(deadAt < p.Messages().size());
    REQUIRE(hostedAt < p.Messages().size());
    REQUIRE(deadAt < hostedAt);
    // the grace passes: the caster's game, which has the cell loaded, hosts
    // it next and is named the body's simulator; the record stays dead
    p.Messages().clear();
    p.TickCellHosts(FateNowMs() + skymp::rules::host_grace_ms() + 2'000);
    REQUIRE(SentOfType(p, 1, MsgType::HostGrant) >= 1);
    REQUIRE(p.SimulatorOf(body) == kCaster);
    REQUIRE(p.worldState.HostOf(body) == &caster);
    REQUIRE(hostedFalseTo(1) >= 1);
    REQUIRE(body.IsDead());
    // the shared cleanup below disconnects the host again
    DoConnect(p, 0);
    p.SetUserActor(0, kHost);
  }

  // the shared world: no claim on the body, the body stood up again with no
  // deadline, the fight a hit may have begun forgotten, the players gone
  p.ReleaseAnyClaim(body);
  body.Respawn(false);
  p.worldState.fateRespawnDue.erase(kPredator);
  p.GetFights().forget(kCaster);
  p.GetFights().forget(kHost);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHost);
  p.DestroyActor(kCaster);
}

// thuum docs/verbs/npc-fate.md (ADR-060 as amended 2026-11-12; C4, the full
// thrall's return): the reanimate case's Dead Thrall sections, on a body
// Dead Thrall's Reanimate effect applies to. ReanimateThrallFFAimed 0x7E8E0
// wants HasKeyword ActorTypeNPC (Skyrim.esm 0x13794) == 1 on Subject beside
// MagicNoReanimate == 0 (thuum lab/.cache/drafts/reanimate-fx.md section 1;
// the server's own row, "HasKeyword(0x13794, 0x0) on Subject == 1", in
// pipeline 5939's log), so on the forest's leveled predator, an animal, the
// stand-up's ApplyEffects (ADR-039) put no Reanimate state on the record and
// the return's GetReanimateCaster named nobody (pipeline 5939, job 21195:
// "0 == 4278193462 (0xff000d36)" at both return sections). The body here is
// a server-made Nord bandit (base EncBandit01Melee2HBerserkNordM 0x3DE6F,
// NordRace), the full thrall's look case's below, whose stand-up there puts
// 0x7E8E0 on the record for its 86,313,600 s; it lies where the reanimate
// case sets the predator, in the forest's cell 0x9810, between that case's
// host and caster
TEST_CASE("A full thrall (Dead Thrall) stays at its commander's disconnect "
          "and is its caster's again at the caster's return, on a body its "
          "Reanimate effect's conditions take",
          "[NpcFate][Reanimate][espm]")
{
  constexpr uint32_t kHost = 0xff000d35;
  constexpr uint32_t kCaster = 0xff000d36;
  constexpr uint32_t kBody = 0xff000d3d;
  constexpr uint32_t kBanditBase = 0x3de6f;
  constexpr uint32_t kDeadThrall = 0x7e8df;
  constexpr uint32_t kDeadThrallEffect = 0x7e8e0;
  // Skyrim.esm's ActorTypeNPC, the keyword of 0x7E8E0's HasKeyword row (the
  // row's own parameter in pipeline 5939's log; thuum
  // docs/verbs/natures-balance.md, NordRaceVampire's KWDA)
  constexpr uint32_t kActorTypeNpc = 0x13794;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;

  // the record itself: Dead Thrall's Reanimate effect is 0x7E8E0, kept as
  // the stand-up applies it
  std::optional<espm::Effects::Effect> thrallEffect;
  for (const auto& effect : MpActor::EffectsOf(&p.worldState, kDeadThrall)) {
    if (espm::GetData<espm::MGEF>(effect.effectId, &p.worldState)
          .data.effectType == espm::MGEF::EffectType::Reanimate) {
      REQUIRE(effect.effectId == kDeadThrallEffect);
      thrallEffect = effect;
    }
  }
  REQUIRE(thrallEffect.has_value());

  // the host takes the forest's cell, the caster has it loaded too (a
  // command needs its commander to have the body's cell loaded, cells.rs
  // claim_commanded; the full thrall's look case's order); the bandit lies
  // dead beside them, hosted by user 0
  DoConnect(p, 0);
  p.CreateActor(kHost, kForest, 0, kTamriel);
  p.SetUserActor(0, kHost);
  HostGrants(p, 0, kForestCell);
  DoConnect(p, 1);
  p.CreateActor(kCaster, kForest, 0, kTamriel);
  p.SetUserActor(1, kCaster);
  {
    RawMessageData raw;
    raw.userId = 1;
    CellLoadedMessage loaded;
    loaded.cell = kForestCell;
    loaded.loaded = true;
    p.GetActionListener().OnCellLoaded(raw, loaded);
  }
  MpChangeForm saved;
  saved.recType = MpChangeForm::ACHR;
  saved.formDesc = FormDesc::FromFormId(kBody, p.worldState.espmFiles);
  saved.baseDesc = FormDesc::FromFormId(kBanditBase, p.worldState.espmFiles);
  saved.worldOrCellDesc = FormDesc::Tamriel();
  saved.position = kForest + NiPoint3{ 100.f, 0.f, 0.f };
  saved.angle = { 0.f, 0.f, 0.f };
  p.worldState.LoadChangeForm(saved, p.CreateFormCallbacks());
  p.worldState.LoadDeferredActors();
  auto& form = p.worldState.LookupFormByIdNoLoad(kBody);
  REQUIRE(form);
  MpActor* bodyPtr = form->AsActor();
  REQUIRE(bodyPtr);
  MpActor& body = *bodyPtr;
  p.worldState.fateRespawnDue.erase(kBody);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  REQUIRE(p.worldState.CellOf(body) == kForestCell);
  REQUIRE(p.worldState.HostOf(body) == &host);

  // every section's premise (Catch2 runs this setup ahead of each one): the
  // body carries ActorTypeNPC, and 0x7E8E0's conditions hold on it for this
  // caster as the stand-up's ApplyEffects reads them (EffectConditionsHold,
  // ADR-039), so the stand-up puts the Reanimate state naming the caster on
  // the record, the row the return reads
  REQUIRE(
    ConditionFunctions::HasKeyword::ActorHasKeyword(body, kActorTypeNpc));
  REQUIRE(MpActor::EffectConditionsHold(&p.worldState, *thrallEffect,
                                        kDeadThrall, body, caster));
  body.Kill(nullptr);
  REQUIRE(body.IsDead());

  // `user`'s cast of `spell` through the packet path, as its client's
  // message arrives, the target the body (the reanimate case's casts)
  const auto casts = [&](Networking::UserId user, uint32_t spell) {
    DoMessage(p, user,
              nlohmann::json{
                { "t", static_cast<int>(MsgType::SpellCast) },
                { "data",
                  { { "caster", 0x14 },
                    { "target", kBody },
                    { "spell", spell },
                    { "isDualCasting", false },
                    { "interruptCast", false },
                    { "castingSource", 1 },
                    { "aimAngle", 0.f },
                    { "aimHeading", 0.f },
                    { "hasLaunch", false },
                    { "originX", 0.f },
                    { "originY", 0.f },
                    { "originZ", 0.f },
                    { "launchAngleX", 0.f },
                    { "launchAngleZ", 0.f },
                    { "actorAnimationVariables",
                      { { "booleans", nlohmann::json::array() },
                        { "floats", nlohmann::json::array() },
                        { "integers", nlohmann::json::array() } } } } } });
  };
  // `user`'s report of its own hit on the body with `spell`
  const auto hits = [&](Networking::UserId user, uint32_t spell) {
    RawMessageData raw;
    raw.userId = user;
    HitMessage hit;
    hit.data.aggressor = 0x14;
    hit.data.target = kBody;
    hit.data.source = spell;
    p.GetActionListener().OnHit(raw, hit);
  };

  // UpdateProperty isHostedByOther for the body sent to `user` since the
  // last clear (UpdateHoster's word)
  const auto hosterWordsTo = [&](Networking::UserId user) {
    size_t count = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::UpdateProperty) &&
          m.j.contains("propName") && m.j["propName"] == "isHostedByOther" &&
          m.j.contains("idx") && m.j["idx"] == body.GetIdx()) {
        ++count;
      }
    }
    return count;
  };

  // the same word saying this game simulates it (isHostedByOther false)
  const auto hostedFalseTo = [&](Networking::UserId user) {
    size_t count = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::UpdateProperty) &&
          m.j.contains("propName") && m.j["propName"] == "isHostedByOther" &&
          m.j.contains("idx") && m.j["idx"] == body.GetIdx() &&
          m.j.value("dataDump", std::string()) == "false") {
        ++count;
      }
    }
    return count;
  };

  // the MagicEffects words for the body sent to `user` since the last clear
  // (MagicEffectsTest's idiom)
  const auto effectWordsTo = [&](Networking::UserId user) {
    std::vector<nlohmann::json> out;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::MagicEffects) &&
          m.j.contains("idx") && m.j["idx"] == body.GetIdx()) {
        out.push_back(m.j);
      }
    }
    return out;
  };

  // the commander's engine stands the thrall: its live sample of the body,
  // the death past the grace and the reanimate's hit inside its window
  // (OnHostedActorState's resurrection branch, PartOne::ResurrectByHost)
  const auto standsOn = [&](Networking::UserId user) {
    body.RecordDeathAt(
      std::chrono::steady_clock::now() -
      std::chrono::milliseconds(MpActor::kPendingDeathMs + 1000));
    State(p, user, kBody, 1.f);
  };

  SECTION("by a player who is not the host: a full thrall (Dead Thrall, a "
          "day and more on the record) stays alive at its commander's "
          "disconnect, the cell's host simulating it, and no clock ends it "
          "(Eli, 2026-11-12: \"if it's a full thrall at 999 days, should "
          "just stay\")")
  {
    const uint32_t seconds = thrallEffect->duration;
    REQUIRE(static_cast<float>(seconds) >= skymp::rules::full_thrall_min_s());
    Equipment eq;
    eq.rightSpell = kDeadThrall;
    caster.SetEquipment(eq);
    casts(1, kDeadThrall);
    hits(1, kDeadThrall);
    REQUIRE(p.CommanderOf(body) == kCaster);
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    // the stand-up's Reanimate state on the record names its caster
    REQUIRE(body.GetReanimateCaster().caster == kCaster);
    p.Messages().clear();
    DoDisconnect(p, 1);
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
    REQUIRE(p.worldState.HostOf(body) == &host);
    REQUIRE(hostedFalseTo(0) >= 1);
    // past the record's own duration, 999 days: still alive
    p.TickCellHosts(FateNowMs() + static_cast<uint64_t>(seconds) * 1000 +
                    1'000);
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.SimulatorOf(body) == kHost);
    // the shared cleanup below disconnects the caster again
    DoConnect(p, 1);
    p.SetUserActor(1, kCaster);
  }

  SECTION("by a player who is not the host: a full thrall that stayed at its "
          "commander's disconnect is its caster's again when the caster's "
          "next game reports the thrall's cell loaded, the record its "
          "dormant row and the host keeping the cell (C4; Eli, 2026-11-12: "
          "\"thrall returns to caster obvs\")")
  {
    Equipment eq;
    eq.rightSpell = kDeadThrall;
    caster.SetEquipment(eq);
    casts(1, kDeadThrall);
    hits(1, kDeadThrall);
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == kCaster);
    // the record names its caster: the dormant row
    REQUIRE(body.GetReanimateCaster().caster == kCaster);
    REQUIRE(body.GetReanimateCaster().recordSeconds >=
            skymp::rules::full_thrall_min_s());
    DoDisconnect(p, 1);
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
    REQUIRE(body.GetReanimateCaster().caster == kCaster);

    // the caster's next login: the body's set names it (MagicEffects'
    // caster), and nothing returns before its game reports the cell loaded
    LogCapture log;
    p.Messages().clear();
    DoConnect(p, 1);
    p.SetUserActor(1, kCaster);
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
    REQUIRE(hostedFalseTo(1) == 0);
    bool named = false;
    for (auto& word : effectWordsTo(1)) {
      for (auto& effect : word["effects"]) {
        if (effect["effect"] == kDeadThrallEffect) {
          REQUIRE(effect["caster"] == kCaster);
          REQUIRE(effect["remaining"].get<float>() == -1.f);
          named = true;
        }
      }
    }
    REQUIRE(named);

    // its game reports the thrall's cell loaded, the host still hosting it:
    // the caster commands the thrall again, both games hear the word, and
    // the host keeps its cell
    const auto loadsTheCell = [&] {
      RawMessageData raw;
      raw.userId = 1;
      CellLoadedMessage loaded;
      loaded.cell = kForestCell;
      loaded.loaded = true;
      p.GetActionListener().OnCellLoaded(raw, loaded);
    };
    p.Messages().clear();
    loadsTheCell();
    REQUIRE(p.CommanderOf(body) == kCaster);
    REQUIRE(p.SimulatorOf(body) == kCaster);
    REQUIRE(p.worldState.HostOf(body) == &caster);
    REQUIRE(p.GetCellHost(kForestCell).player == kHost);
    REQUIRE(hostedFalseTo(1) >= 1);
    REQUIRE(hosterWordsTo(0) >= 1);
    REQUIRE(
      log.Count("ff000d3d returns to its caster ff000d36 in cell 9810: a "
                "full thrall its record names") == 1);
    // a later load claims nothing again: its own command stands
    loadsTheCell();
    REQUIRE(log.Count("returns to its caster ff000d36") == 1);
    REQUIRE(p.CommanderOf(body) == kCaster);
    // the returned command is the cast's own: the thrall's death ends it
    body.Kill(nullptr);
    REQUIRE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
  }

  SECTION("by a player who is not the host: a full thrall killed while its "
          "caster was away is no thrall at the caster's return (C4)")
  {
    Equipment eq;
    eq.rightSpell = kDeadThrall;
    caster.SetEquipment(eq);
    casts(1, kDeadThrall);
    hits(1, kDeadThrall);
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    // the record names its caster: the death below, not a missing state, is
    // why no return comes
    REQUIRE(body.GetReanimateCaster().caster == kCaster);
    DoDisconnect(p, 1);
    REQUIRE_FALSE(body.IsDead());
    // the host's engine kills it while its caster is away
    body.Kill(nullptr);
    REQUIRE(body.IsDead());
    LogCapture log;
    DoConnect(p, 1);
    p.SetUserActor(1, kCaster);
    {
      RawMessageData raw;
      raw.userId = 1;
      CellLoadedMessage loaded;
      loaded.cell = kForestCell;
      loaded.loaded = true;
      p.GetActionListener().OnCellLoaded(raw, loaded);
    }
    REQUIRE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
    REQUIRE(log.Count("returns to its caster") == 0);
  }

  SECTION("by a player who is not the host: a full thrall served to its "
          "caster's next game after that game reported the thrall's cell "
          "loaded is claimed at the subscribe, once, its create word naming "
          "the caster its simulator (C4: the engine's 5x5 runs ahead of the "
          "server's 3x3)")
  {
    Equipment eq;
    eq.rightSpell = kDeadThrall;
    caster.SetEquipment(eq);
    casts(1, kDeadThrall);
    hits(1, kDeadThrall);
    standsOn(1);
    REQUIRE_FALSE(body.IsDead());
    // the stand-up's Reanimate state on the record names its caster
    REQUIRE(body.GetReanimateCaster().caster == kCaster);
    DoDisconnect(p, 1);
    REQUIRE_FALSE(body.IsDead());
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(body.GetReanimateCaster().caster == kCaster);
    // the caster's next game starts three grid squares east: the body is no
    // emitter of its, so its load of the body's cell meets no thrall
    caster.SetPos(kForest + NiPoint3{ 3 * 4096.f, 0.f, 0.f });
    LogCapture log;
    DoConnect(p, 1);
    p.SetUserActor(1, kCaster);
    REQUIRE(caster.GetEmitters().count(&body) == 0);
    const auto loadsTheCell = [&] {
      RawMessageData raw;
      raw.userId = 1;
      CellLoadedMessage loaded;
      loaded.cell = kForestCell;
      loaded.loaded = true;
      p.GetActionListener().OnCellLoaded(raw, loaded);
    };
    loadsTheCell();
    REQUIRE(p.CommanderOf(body) == 0);
    REQUIRE(p.SimulatorOf(body) == kHost);
    REQUIRE(log.Count("returns to its caster ff000d36") == 0);
    // it comes back beside the body: the grid serves the body to its game,
    // and the subscribe claims it ahead of the create word
    p.Messages().clear();
    caster.SetPos(kForest + NiPoint3{ 50.f, 0.f, 0.f });
    REQUIRE(caster.GetEmitters().count(&body) == 1);
    REQUIRE(p.CommanderOf(body) == kCaster);
    REQUIRE(p.SimulatorOf(body) == kCaster);
    REQUIRE(
      log.Count("ff000d3d returns to its caster ff000d36 in cell 9810") == 1);
    bool created = false;
    for (auto& m : p.Messages()) {
      if (m.userId == 1 && m.j.is_object() &&
          m.j.value("t", -1) == static_cast<int>(MsgType::CreateActor) &&
          m.j.contains("refrId") && m.j["refrId"] == kBody) {
        REQUIRE(m.j["props"]["isHostedByOther"] == false);
        created = true;
      }
    }
    REQUIRE(created);
    // a later load claims nothing again: its own command stands
    loadsTheCell();
    REQUIRE(log.Count("returns to its caster ff000d36") == 1);
    REQUIRE(p.CommanderOf(body) == kCaster);
  }

  // the shared world: no claim on the body, which goes, the fight a hit may
  // have begun forgotten, the players gone
  p.ReleaseAnyClaim(body);
  body.Respawn(false);
  p.worldState.fateRespawnDue.erase(kBody);
  p.GetFights().forget(kCaster);
  p.GetFights().forget(kHost);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kBody);
  p.DestroyActor(kHost);
  p.DestroyActor(kCaster);
}

// thuum docs/verbs/npc-fate.md (ADR-060; the fresh review of f89115af, C3):
// a thrall's end its commander reports is the commander's word (R2), and
// the engine's essential and protected rules (MpActor::MayDie) are a check
// on the owner there, as on a host-run script's kill (ADR-063 decision 4);
// before, a protected base's thrall had that report refused
// E_DEATH_IMMORTAL and, with no lease, no end at all. Jenassa 0xe1ba9 (base
// HirelingJenassa 0xb9982, protected and not essential in Skyrim.esm, read
// here from the record) in WhiterunDrunkenHuntsman 0x165b7, hosted by user
// 0 and raised by user 1
TEST_CASE("A protected base's thrall ends at its commander's report of the "
          "effect's end",
          "[NpcFate][Reanimate][espm]")
{
  constexpr uint32_t kJenassa = 0xe1ba9;
  constexpr uint32_t kInn = 0x165b7;
  constexpr uint32_t kHost = 0xff000d38;
  constexpr uint32_t kCaster = 0xff000d39;
  constexpr uint32_t kReanimateCorpse = 0x65bd7;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.IsInteriorCell(kInn));
  auto& body = p.worldState.GetFormAt<MpActor>(kJenassa);
  const auto npc = espm::GetData<espm::NPC_>(body.GetBaseId(), &p.worldState);
  REQUIRE(npc.isProtected);
  REQUIRE_FALSE(npc.isEssential);
  REQUIRE(body.IsFateRespawned());
  REQUIRE(p.worldState.CellOf(body) == kInn);
  if (body.IsDead()) {
    body.Respawn(false);
  }

  // the host stands in the inn and takes it; the caster beside her has the
  // inn loaded too, as its client reports it
  DoConnect(p, 0);
  p.CreateActor(kHost, body.GetPos() + NiPoint3{ 100.f, 0.f, 0.f }, 0, kInn);
  p.SetUserActor(0, kHost);
  HostGrants(p, 0, kInn);
  DoConnect(p, 1);
  p.CreateActor(kCaster, body.GetPos() + NiPoint3{ -100.f, 0.f, 0.f }, 0,
                kInn);
  p.SetUserActor(1, kCaster);
  {
    RawMessageData raw;
    raw.userId = 1;
    CellLoadedMessage loaded;
    loaded.cell = kInn;
    loaded.loaded = true;
    p.GetActionListener().OnCellLoaded(raw, loaded);
  }
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  REQUIRE(p.worldState.HostOf(body) == &host);
  body.Kill(nullptr);
  REQUIRE(body.IsDead());

  // the caster's Reanimate Corpse at her body, as its client's messages
  // arrive: the cast, then its hit
  Equipment eq;
  eq.rightSpell = kReanimateCorpse;
  caster.SetEquipment(eq);
  DoMessage(
    p, 1,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", kJenassa },
                        { "spell", kReanimateCorpse },
                        { "isDualCasting", false },
                        { "interruptCast", false },
                        { "castingSource", 1 },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "hasLaunch", false },
                        { "originX", 0.f },
                        { "originY", 0.f },
                        { "originZ", 0.f },
                        { "launchAngleX", 0.f },
                        { "launchAngleZ", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
  {
    RawMessageData raw;
    raw.userId = 1;
    HitMessage hit;
    hit.data.aggressor = 0x14;
    hit.data.target = kJenassa;
    hit.data.source = kReanimateCorpse;
    p.GetActionListener().OnHit(raw, hit);
  }
  REQUIRE(p.CommanderOf(body) == kCaster);

  // the commander's engine stands her (its live sample, the death past the
  // grace): alive and still commanded, a death the engine's rule refuses to
  // any hand but a player's
  body.RecordDeathAt(
    std::chrono::steady_clock::now() -
    std::chrono::milliseconds(MpActor::kPendingDeathMs + 1000));
  State(p, 1, kJenassa, 1.f);
  REQUIRE_FALSE(body.IsDead());
  REQUIRE(p.CommanderOf(body) == kCaster);
  REQUIRE_FALSE(body.MayDie(nullptr));

  // its engine ends the effect: the commander's cause-0 report, the hit and
  // the stand-up older than every other evidence's window
  const auto now = std::chrono::steady_clock::now();
  body.NoteHitBy(kCaster, now - std::chrono::seconds(130));
  body.NoteHostStoodUp(now -
                       std::chrono::milliseconds(MpActor::kReanimatedEndMs) -
                       std::chrono::seconds(30));
  const bool reportsBefore = p.worldState.npcDeathReports;
  p.worldState.npcDeathReports = true;
  {
    ActorDeathMessage report;
    report.refrId = kJenassa;
    report.cause = 0;
    report.killer = 0;
    const auto& at = body.GetPos();
    report.pos = { at.x, at.y, at.z };
    RawMessageData raw;
    raw.userId = 1;
    p.GetActionListener().OnActorDeath(raw, report);
  }
  p.worldState.npcDeathReports = reportsBefore;
  REQUIRE(body.IsDead());
  REQUIRE(p.CommanderOf(body) == 0);
  REQUIRE(p.SimulatorOf(body) == kHost);

  // the shared world: no claim on her, standing again with no deadline, the
  // fight a hit may have begun forgotten, the players gone
  p.ReleaseAnyClaim(body);
  body.Respawn(false);
  p.worldState.fateRespawnDue.erase(kJenassa);
  p.GetFights().forget(kCaster);
  p.GetFights().forget(kHost);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHost);
  p.DestroyActor(kCaster);
}

// thuum docs/verbs/npc-fate.md (ADR-060 as amended 2026-11-12, Eli: "if
// it's a full thrall at 999 days, should just stay. i want it to keep it's
// undead look though"): Dead Thrall 0x7E8DF's ReanimateThrallFFAimed
// 0x7E8E0 lasts 86,313,600 s in Skyrim.esm (a scratch reader over thuum
// lab/esm.py, 2026-11-12), a full thrall's by wire-rules
// magic::FULL_THRALL_MIN_S: the record keeps that duration whole, every game
// hears the look from the stand-up, and the commander's departure ends
// neither the thrall nor its look on the host's game or a later arrival's;
// its death's emptied set ends it. The effect's own conditions want
// ActorTypeNPC, so the body is a server-made Nord bandit (base
// EncBandit01Melee2HBerserkNordM 0x3DE6F, NordRace), the dead server-made
// NPC case's.
TEST_CASE("A full thrall's look is the record's for the record's whole "
          "duration and outlasts its commander's departure; its death ends "
          "it",
          "[NpcFate][Reanimate][espm]")
{
  constexpr uint32_t kHost = 0xff000d38;
  constexpr uint32_t kCaster = 0xff000d39;
  constexpr uint32_t kThrall = 0xff000d3a;
  constexpr uint32_t kLater = 0xff000d3b;
  constexpr uint32_t kBanditBase = 0x3de6f;
  constexpr uint32_t kDeadThrall = 0x7e8df;
  constexpr uint32_t kThrallEffect = 0x7e8e0;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;

  // the record itself: Dead Thrall's Reanimate effect and its duration
  float recordSeconds = 0.f;
  for (const auto& effect : MpActor::EffectsOf(&p.worldState, kDeadThrall)) {
    if (espm::GetData<espm::MGEF>(effect.effectId, &p.worldState)
          .data.effectType == espm::MGEF::EffectType::Reanimate) {
      REQUIRE(effect.effectId == kThrallEffect);
      recordSeconds = static_cast<float>(effect.duration);
    }
  }
  REQUIRE(recordSeconds == 86'313'600.f);

  // the host takes the forest's cell, the caster has it loaded too; the
  // bandit lies dead beside them, hosted by user 0
  DoConnect(p, 0);
  p.CreateActor(kHost, kForest, 0, kTamriel);
  p.SetUserActor(0, kHost);
  HostGrants(p, 0, kForestCell);
  DoConnect(p, 1);
  p.CreateActor(kCaster, kForest, 0, kTamriel);
  p.SetUserActor(1, kCaster);
  {
    RawMessageData raw;
    raw.userId = 1;
    CellLoadedMessage loaded;
    loaded.cell = kForestCell;
    loaded.loaded = true;
    p.GetActionListener().OnCellLoaded(raw, loaded);
  }
  MpChangeForm saved;
  saved.recType = MpChangeForm::ACHR;
  saved.formDesc = FormDesc::FromFormId(kThrall, p.worldState.espmFiles);
  saved.baseDesc = FormDesc::FromFormId(kBanditBase, p.worldState.espmFiles);
  saved.worldOrCellDesc = FormDesc::Tamriel();
  saved.position = kForest + NiPoint3{ 100.f, 0.f, 0.f };
  saved.angle = { 0.f, 0.f, 0.f };
  p.worldState.LoadChangeForm(saved, p.CreateFormCallbacks());
  p.worldState.LoadDeferredActors();
  auto& form = p.worldState.LookupFormByIdNoLoad(kThrall);
  REQUIRE(form);
  MpActor* thrallPtr = form->AsActor();
  REQUIRE(thrallPtr);
  MpActor& thrall = *thrallPtr;
  p.worldState.fateRespawnDue.erase(kThrall);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  REQUIRE(p.worldState.HostOf(thrall) == &host);
  thrall.Kill(nullptr);
  REQUIRE(thrall.IsDead());

  // the MagicEffects words for the thrall sent to `user` since the last
  // clear, every one read (the reanimate case's idiom)
  const auto effectWordsTo = [&](Networking::UserId user) {
    std::vector<nlohmann::json> out;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::MagicEffects) &&
          m.j.contains("idx") && m.j["idx"] == thrall.GetIdx()) {
        out.push_back(m.j);
      }
    }
    return out;
  };
  // the thrall's Reanimate effect's seconds left in a word, nothing when the
  // word names none
  const auto lookIn = [&](nlohmann::json& word) -> std::optional<float> {
    for (auto& effect : word["effects"]) {
      if (effect["effect"] == kThrallEffect) {
        return effect["remaining"].get<float>();
      }
    }
    return std::nullopt;
  };

  // the caster's cast and hit through the packet path, then its engine's
  // stand-up: its live sample past the death's grace, inside the window
  Equipment eq;
  eq.rightSpell = kDeadThrall;
  caster.SetEquipment(eq);
  DoMessage(
    p, 1,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", kThrall },
                        { "spell", kDeadThrall },
                        { "isDualCasting", false },
                        { "interruptCast", false },
                        { "castingSource", 1 },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "hasLaunch", false },
                        { "originX", 0.f },
                        { "originY", 0.f },
                        { "originZ", 0.f },
                        { "launchAngleX", 0.f },
                        { "launchAngleZ", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
  {
    RawMessageData raw;
    raw.userId = 1;
    HitMessage hit;
    hit.data.aggressor = 0x14;
    hit.data.target = kThrall;
    hit.data.source = kDeadThrall;
    p.GetActionListener().OnHit(raw, hit);
  }
  REQUIRE(p.CommanderOf(thrall) == kCaster);
  thrall.RecordDeathAt(
    std::chrono::steady_clock::now() -
    std::chrono::milliseconds(MpActor::kPendingDeathMs + 1000));
  p.Messages().clear();
  State(p, 1, kThrall, 1.f);
  REQUIRE_FALSE(thrall.IsDead());

  // both games hear the look with no end (-1, a state with no clock: thuum
  // docs/WIRE.md, MagicEffects' remaining; the platform review of 3a31c05a,
  // F5); the record keeps its 86,313,600 s, not a day's
  for (const Networking::UserId user : { 0, 1 }) {
    auto words = effectWordsTo(user);
    REQUIRE_FALSE(words.empty());
    for (auto& word : words) {
      REQUIRE(lookIn(word) == -1.f);
    }
  }
  const auto lookOnRecord = [&] {
    for (const auto& e : thrall.GetRunningEffects()) {
      if (e.effect == kThrallEffect) {
        return e.secondsLeft;
      }
    }
    return -1.f;
  };
  REQUIRE(lookOnRecord() == Catch::Approx(recordSeconds));

  // its commander leaves: the thrall stays on the cell's host, alive, its
  // look kept whole on the record; no word ends it on the host's game
  p.Messages().clear();
  DoDisconnect(p, 1);
  REQUIRE(p.CommanderOf(thrall) == 0);
  REQUIRE(p.SimulatorOf(thrall) == kHost);
  REQUIRE_FALSE(thrall.IsDead());
  REQUIRE(effectWordsTo(0).empty());
  REQUIRE(lookOnRecord() == Catch::Approx(recordSeconds));

  // a later arrival's game first sees the thrall and hears the look whole
  // (the subscription's SendMagicEffectsTo)
  DoConnect(p, 2);
  p.CreateActor(kLater, kForest, 0, kTamriel);
  p.SetUserActor(2, kLater);
  {
    auto words = effectWordsTo(2);
    REQUIRE_FALSE(words.empty());
    for (auto& word : words) {
      REQUIRE(lookIn(word) == -1.f);
    }
  }

  // its death: the emptied set to every game that sees it
  p.Messages().clear();
  thrall.Kill(nullptr);
  REQUIRE(thrall.IsDead());
  thrall.AdvanceEffects(0.25f);
  REQUIRE_FALSE(thrall.HasRunningEffects());
  for (const Networking::UserId user : { 0, 2 }) {
    auto words = effectWordsTo(user);
    REQUIRE_FALSE(words.empty());
    for (auto& word : words) {
      REQUIRE(word["effects"].empty());
    }
  }

  // the shared world
  p.ReleaseAnyClaim(thrall);
  thrall.Respawn(false);
  p.worldState.fateRespawnDue.erase(kThrall);
  p.GetFights().forget(kCaster);
  p.GetFights().forget(kHost);
  DoDisconnect(p, 0);
  DoDisconnect(p, 2);
  p.DestroyActor(kThrall);
  p.DestroyActor(kHost);
  p.DestroyActor(kCaster);
  p.DestroyActor(kLater);
}

// thuum docs/verbs/npc-fate.md (ADR-060 as amended 2026-11-12): a restart is
// every commander's departure, the cells rule's commands living in memory
// alone, so a thrall its saved record brings back alive with its Reanimate
// state (MpActor::ApplyChangeForm's ReapplyMagicEffects, at the load) is
// judged by the state's record duration: a timed thrall dies by the server's
// word at the world's next tick, a full thrall stays with its look. The
// record is a server-made Nord bandit's (base 0x3DE6F), loaded through
// WorldState::LoadChangeForm as the boot loads a saved one
TEST_CASE("At a restart a timed thrall's record dies by the server's word "
          "and a full thrall's stays with its look",
          "[NpcFate][Reanimate][espm]")
{
  constexpr uint32_t kThrall = 0xff000d3c;
  constexpr uint32_t kBanditBase = 0x3de6f;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  const auto wallNowMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count());

  // the saved record: alive at the forest, its reanimate state 5 s in as of
  // now, kept as a state with its key
  const auto load = [&](uint32_t effect, uint32_t spell, float magnitude,
                        float recordSeconds) -> MpActor& {
    MpChangeForm saved;
    saved.recType = MpChangeForm::ACHR;
    saved.formDesc = FormDesc::FromFormId(kThrall, p.worldState.espmFiles);
    saved.baseDesc = FormDesc::FromFormId(kBanditBase, p.worldState.espmFiles);
    saved.worldOrCellDesc = FormDesc::Tamriel();
    saved.position = kForest;
    saved.angle = { 0.f, 0.f, 0.f };
    RunningEffect state;
    state.effect = FormDesc::FromFormId(effect, p.worldState.espmFiles);
    state.source = FormDesc::FromFormId(spell, p.worldState.espmFiles);
    state.kind = 3;
    state.reanimate = true;
    state.magnitude = magnitude;
    state.durationS = recordSeconds;
    state.elapsedS = 5.f;
    state.asOfMs = wallNowMs;
    state.recover = true;
    saved.runningEffects = std::vector<RunningEffect>{ state };
    p.worldState.LoadChangeForm(saved, p.CreateFormCallbacks());
    p.worldState.LoadDeferredActors();
    p.worldState.fateRespawnDue.erase(kThrall);
    return p.worldState.GetFormAt<MpActor>(kThrall);
  };

  SECTION("Reanimate Corpse's 0x96D0B, the record's 60 s: a timed thrall, "
          "the server's word ends it once the load is done")
  {
    LogCapture log;
    auto& thrall = load(0x96d0b, 0x65bd7, 13.f, 60.f);
    REQUIRE_FALSE(thrall.IsDead());
    // the items it carries, every entry's count
    const auto items = [&] {
      uint32_t n = 0;
      for (const auto& entry : thrall.GetInventory().entries) {
        n += entry.count;
      }
      return n;
    };
    const uint32_t carried = items();
    p.Tick();
    REQUIRE(thrall.IsDead());
    // the restart's kill of a standing thrall rolls no death item (single
    // player's rule)
    REQUIRE(items() == carried);
    REQUIRE(log.Count("dies from its reanimation: no death item rolled") == 1);
    REQUIRE(log.Count("commander left at the server's restart and its effect "
                      "is a timed thrall's (the record's 60 s): the server's "
                      "word ends it") == 1);
  }

  SECTION("Dead Thrall's 0x7E8E0, the record's 86,313,600 s: a full thrall "
          "stays, its look kept")
  {
    LogCapture log;
    auto& thrall = load(0x7e8e0, 0x7e8df, 40.f, 86'313'600.f);
    p.Tick();
    REQUIRE_FALSE(thrall.IsDead());
    REQUIRE(log.Count("commander left at the server's restart and its effect "
                      "is a full thrall's (the record's 86313600 s): it "
                      "stays, its look kept") == 1);
    const auto running = thrall.GetRunningEffects();
    REQUIRE(running.size() == 1);
    REQUIRE(running[0].effect == 0x7e8e0);
    REQUIRE(running[0].secondsLeft == Catch::Approx(86'313'595.f));
  }

  // the shared world
  auto& thrall = p.worldState.GetFormAt<MpActor>(kThrall);
  thrall.Respawn(false);
  p.worldState.fateRespawnDue.erase(kThrall);
  p.DestroyActor(kThrall);
}

// thuum docs/verbs/npc-fate.md C4 (ADR-060's return; rule 6's option A,
// 2026-11-12): the dormant row is the record, so a restart keeps it. A full
// thrall's saved record that names its caster, written to the change form's
// JSON and read back into a fresh world (the database's round trip, a
// restart), stays alive with its look at the load and is its caster's again
// once the caster's game reports the thrall's cell loaded, whoever hosts
// the cell; another player's load claims nothing
TEST_CASE("After a restart a full thrall's record still names its caster, "
          "and the caster's return commands it again (C4)",
          "[NpcFate][Reanimate][espm]")
{
  constexpr uint32_t kThrall = 0xff000d3e;
  constexpr uint32_t kCaster = 0xff000d3f;
  constexpr uint32_t kOther = 0xff000d40;
  constexpr uint32_t kBanditBase = 0x3de6f;
  constexpr uint32_t kDeadThrall = 0x7e8df;
  constexpr uint32_t kDeadThrallEffect = 0x7e8e0;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  const auto wallNowMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count());

  // the saved record: alive at the forest, Dead Thrall's state 5 s in as of
  // now, kept as a state with its key and its caster
  MpChangeForm saved;
  saved.recType = MpChangeForm::ACHR;
  saved.formDesc = FormDesc::FromFormId(kThrall, p.worldState.espmFiles);
  saved.baseDesc = FormDesc::FromFormId(kBanditBase, p.worldState.espmFiles);
  saved.worldOrCellDesc = FormDesc::Tamriel();
  saved.position = kForest;
  saved.angle = { 0.f, 0.f, 0.f };
  RunningEffect state;
  state.effect =
    FormDesc::FromFormId(kDeadThrallEffect, p.worldState.espmFiles);
  state.source = FormDesc::FromFormId(kDeadThrall, p.worldState.espmFiles);
  state.caster = FormDesc::FromFormId(kCaster, p.worldState.espmFiles);
  state.kind = 3;
  state.reanimate = true;
  state.magnitude = 40.f;
  state.durationS = 86'313'600.f;
  state.elapsedS = 5.f;
  state.asOfMs = wallNowMs;
  state.recover = true;
  saved.runningEffects = std::vector<RunningEffect>{ state };

  // the database's round trip: the change form's JSON and back
  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(saved).dump();
  auto element = parser.parse(dump).value();
  const MpChangeForm reread = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(reread.runningEffects == saved.runningEffects);

  LogCapture log;
  p.worldState.LoadChangeForm(reread, p.CreateFormCallbacks());
  p.worldState.LoadDeferredActors();
  p.worldState.fateRespawnDue.erase(kThrall);
  auto& thrall = p.worldState.GetFormAt<MpActor>(kThrall);
  p.Tick();
  REQUIRE_FALSE(thrall.IsDead());
  REQUIRE(thrall.GetReanimateCaster().caster == kCaster);
  REQUIRE(log.Count("is a full thrall's (the record's 86313600 s): it stays, "
                    "its look kept, until its caster ff000d3f returns") == 1);

  // another player takes the forest's cell first: its load claims nothing
  DoConnect(p, 0);
  p.CreateActor(kOther, kForest, 0, kTamriel);
  p.SetUserActor(0, kOther);
  HostGrants(p, 0, kForestCell);
  REQUIRE(p.CommanderOf(thrall) == 0);
  REQUIRE(p.GetCellHost(kForestCell).player == kOther);

  // the caster returns: commanded once its game reports the cell loaded
  DoConnect(p, 1);
  p.CreateActor(kCaster, kForest + NiPoint3{ 50.f, 0.f, 0.f }, 0, kTamriel);
  p.SetUserActor(1, kCaster);
  REQUIRE(p.CommanderOf(thrall) == 0);
  {
    RawMessageData raw;
    raw.userId = 1;
    CellLoadedMessage loaded;
    loaded.cell = kForestCell;
    loaded.loaded = true;
    p.GetActionListener().OnCellLoaded(raw, loaded);
  }
  REQUIRE(p.CommanderOf(thrall) == kCaster);
  REQUIRE(p.SimulatorOf(thrall) == kCaster);
  REQUIRE(p.GetCellHost(kForestCell).player == kOther);
  REQUIRE(log.Count("returns to its caster ff000d3f in cell 9810") == 1);

  // the shared world
  p.ReleaseAnyClaim(thrall);
  thrall.Respawn(false);
  p.worldState.fateRespawnDue.erase(kThrall);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kOther);
  p.DestroyActor(kCaster);
  p.DestroyActor(kThrall);
}

// thuum docs/verbs/npc-fate.md (playtest 38's finding 65; the train's
// review, 2026-10-29): a placement that starts dead is the server's corpse
// with no death this process stamped, so the resurrection rule's no-stamp
// reading (ActorDeathTest, finding 54) would stand it at its host's first
// sample above one point: the engine's own values on a body it spawned
// dead. A placed corpse stands on the record by the cast's rule alone: its
// host's live sample within MpActor::kReanimateWindowMs of a reanimate hit
// the server accepted on it
TEST_CASE("A placed-dead body ignores its host's live sample until the "
          "server accepts a reanimate on it: no resurrection by a sample "
          "alone, no respawn scheduled (finding 65)",
          "[NpcFate][espm]")
{
  constexpr uint32_t kFloorBody = 0x3b9ea;
  constexpr uint32_t kBarrow = 0x371de;
  constexpr uint32_t kHost = 0xff000d37;
  constexpr uint32_t kReanimateCorpse = 0x65bd7;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.IsInteriorCell(kBarrow));
  auto& body = p.worldState.GetFormAt<MpActor>(kFloorBody);
  REQUIRE(body.IsDead());
  REQUIRE(body.IsPlacedDead());
  REQUIRE(body.IsFateRespawned());
  REQUIRE(p.worldState.CellOf(body) == kBarrow);
  REQUIRE_FALSE(body.DiedAgo(std::chrono::steady_clock::now()).has_value());

  // the host stands in the barrow beside the body and takes the cell
  DoConnect(p, 0);
  p.CreateActor(kHost, body.GetPos() + NiPoint3{ 100.f, 0.f, 0.f }, 0,
                kBarrow);
  p.SetUserActor(0, kHost);
  HostGrants(p, 0, kBarrow);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  REQUIRE(p.worldState.HostOf(body) == &host);

  // the live words sent for the body since the last clear
  const auto liveWords = [&]() {
    size_t n = 0;
    for (auto& m : p.Messages()) {
      if (m.j.is_object() &&
          m.j.value("t", -1) == static_cast<int>(MsgType::UpdateProperty) &&
          m.j.value("propName", std::string()) == "isDead" &&
          m.j.value("refrId", 0u) == kFloorBody &&
          m.j.value("dataDump", std::string()) == "false") {
        ++n;
      }
    }
    return n;
  };

  // the host's live sample alone: the record stays the corpse, nothing
  // scheduled, no live word
  p.Messages().clear();
  State(p, 0, kFloorBody, 0.9f);
  REQUIRE(body.IsDead());
  REQUIRE(body.IsPlacedDead());
  REQUIRE_FALSE(body.IsRespawning());
  REQUIRE(p.worldState.fateRespawnDue.count(kFloorBody) == 0);
  REQUIRE(body.GetChangeForm().actorValues.healthPercentage == 0.f);
  REQUIRE(liveWords() == 0);

  // a reanimate the server accepted on it (the host's own cast, then its
  // hit: OnHit's stamp ahead of ClaimReanimated, whose commanded claim names
  // the host's own seat, the simulator already): the next live sample is the
  // engine's stand-up, recorded with the live word
  Equipment eq;
  eq.rightSpell = kReanimateCorpse;
  host.SetEquipment(eq);
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", kFloorBody },
                        { "spell", kReanimateCorpse },
                        { "isDualCasting", false },
                        { "interruptCast", false },
                        { "castingSource", 1 },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "hasLaunch", false },
                        { "originX", 0.f },
                        { "originY", 0.f },
                        { "originZ", 0.f },
                        { "launchAngleX", 0.f },
                        { "launchAngleZ", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
  {
    RawMessageData raw;
    raw.userId = 0;
    HitMessage hit;
    hit.data.aggressor = 0x14;
    hit.data.target = kFloorBody;
    hit.data.source = kReanimateCorpse;
    p.GetActionListener().OnHit(raw, hit);
  }
  REQUIRE(
    body.ReanimateAcceptedAgo(std::chrono::steady_clock::now()).has_value());
  p.Messages().clear();
  State(p, 0, kFloorBody, 0.9f);
  REQUIRE_FALSE(body.IsDead());
  REQUIRE_FALSE(body.GetChangeForm().fateDiedAt.has_value());
  REQUIRE(p.worldState.fateRespawnDue.count(kFloorBody) == 0);

  // its next death is still the placement's: no clock stands it again
  body.Kill(nullptr);
  REQUIRE(body.IsDead());
  REQUIRE_FALSE(body.IsRespawning());
  REQUIRE(p.worldState.fateRespawnDue.count(kFloorBody) == 0);

  p.GetFights().forget(kHost);
  DoDisconnect(p, 0);
  p.DestroyActor(kHost);
  p.DestroyActor(kFloorBody);
}

// thuum docs/verbs/npc-fate.md (playtest 44's 1c; x-fate-inventory v1b
// 20261104-062409): MS13BanditCampfire02's placement 0x39fe2 in Bleak Falls
// Temple keeps its own rows (base 0x39fe5, UseInventory unset: an IronDagger,
// LItemBanditWeaponArrows x1 and LItemBanditWeaponBow x1, both lists at
// chanceNone 0 and all levels), yet the probe's record held the dagger alone
// after the fate and lost the 10 gold put before it (finding 1: the roll's
// reset replaced Init's level-0 roll and the put with it). Here Init rolls
// nothing for an actor the fate pass will roll, the put stays, and the roll
// lands beside it at the loader's level: one bow in hand (the best weapon),
// the arrows by the nested count (BaseArrowIron75 x20 at 75 percent each),
// the dagger kept; the fate's rows name everything but the worn bow.
TEST_CASE("The campfire bandit's own rows roll at its fate and a put made "
          "before the roll stays in the record",
          "[NpcFate][espm]")
{
  constexpr uint32_t kBandit = 0x39fe2;
  constexpr uint32_t kBarrow = 0x371de;
  constexpr uint32_t kPlayer = 0xff000d41;
  constexpr uint32_t kGold = 0xf;
  constexpr uint32_t kIronDagger = 0x1397e;
  constexpr uint32_t kIronArrow = 0x1397d;
  constexpr uint32_t kHuntingBow = 0x13985;
  constexpr uint32_t kLongBow = 0x3b562;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.IsInteriorCell(kBarrow));
  auto& bandit = p.worldState.GetFormAt<MpActor>(kBandit);
  REQUIRE(p.worldState.CellOf(bandit) == kBarrow);
  REQUIRE_FALSE(bandit.GetFateLevel().has_value());
  REQUIRE(bandit.CanRollFate());
  // Init rolled nothing for an actor the fate pass will roll
  REQUIRE_FALSE(bandit.GetChangeForm().baseContainerAdded);
  REQUIRE(bandit.GetInventory().GetItemCount(kIronDagger) == 0);

  // the put before the first loader (lab-api's server-console AddItem)
  bandit.AddItem(kGold, 10);
  REQUIRE(bandit.GetInventory().GetItemCount(kGold) == 10);
  REQUIRE(bandit.CanRollFate());

  DoConnect(p, 0);
  p.CreateActor(kPlayer, { -3400.f, 700.f, 583.f }, 0, kBarrow);
  p.SetUserActor(0, kPlayer);
  auto& player = p.worldState.GetFormAt<MpActor>(kPlayer);
  ActorValueRecord record;
  record.level = 1;
  player.SetActorValueRecord(record);
  p.Messages().clear();
  p.TickCellHosts(FateNowMs());

  REQUIRE(bandit.GetFateLevel() == std::optional<uint16_t>{ 1 });
  REQUIRE_FALSE(bandit.CanRollFate());
  const Inventory inv = bandit.GetInventory();
  INFO("gold " << inv.GetItemCount(kGold) << " dagger "
               << inv.GetItemCount(kIronDagger) << " arrows "
               << inv.GetItemCount(kIronArrow) << " hunting "
               << inv.GetItemCount(kHuntingBow) << " long "
               << inv.GetItemCount(kLongBow));
  REQUIRE(inv.GetItemCount(kGold) == 10);
  REQUIRE(inv.GetItemCount(kIronDagger) == 1);
  REQUIRE(inv.GetItemCount(kHuntingBow) + inv.GetItemCount(kLongBow) == 1);
  REQUIRE(inv.GetItemCount(kIronArrow) >= 1);
  REQUIRE(inv.GetItemCount(kIronArrow) <= 20);
  // the best weapon in hand: a bow over the dagger
  REQUIRE((Wearing(bandit, kHuntingBow) || Wearing(bandit, kLongBow)));
  REQUIRE_FALSE(Wearing(bandit, kIronDagger));

  // the fate's rows: the record apart from the worn (the gold, the arrows,
  // the dagger; no bow, it is in hand)
  const auto fates = SentTo<ActorFateMessage>(p, 0);
  const ActorFateMessage::Fate* sent = nullptr;
  for (const auto& msg : fates) {
    for (const auto& f : msg.fates) {
      if (f.refrId == kBandit) {
        sent = &f;
      }
    }
  }
  REQUIRE(sent != nullptr);
  REQUIRE(sent->level == 1);
  auto rowOf = [&](uint32_t baseId) -> uint32_t {
    for (const auto& r : sent->inv) {
      if (r.baseId == baseId) {
        return r.count;
      }
    }
    return 0;
  };
  REQUIRE(rowOf(kGold) == 10);
  REQUIRE(rowOf(kIronDagger) == 1);
  REQUIRE(rowOf(kIronArrow) == inv.GetItemCount(kIronArrow));
  REQUIRE(rowOf(kHuntingBow) + rowOf(kLongBow) == 0);

  DoDisconnect(p, 0);
  p.DestroyActor(kPlayer);
}

// thuum ADR-049 D-N1 A (Eli's "go for it", 2026-11-09; playtest 50's naked
// bandits): a recorded NPC with no fate level that holds a worn item adopts
// its record as its roll: its fateLevel is set at the first loader's level
// and nothing is rolled, so its outfit stands; a recorded one holding no
// worn item is rolled beside what it holds, as before. The two entrance
// bandits of Bleak Falls Temple 0x371de (lab/esm.py on sky-srv, 2026-11-06;
// the campfire case's), each given a record from before the fate: 0x39fe2
// wearing an iron dagger 0x1397e and holding 10 gold, 0x39fe4 holding 10
// gold and wearing nothing. A roll draws from a generator std::random_device
// seeds at every list it rolls (LeveledListUtils.cpp:43-44), so an exact
// count is asserted only where nothing is rolled: the adopted record,
// exactly as it was, which canon's re-roll beside it breaks (new outfit
// entries)
TEST_CASE("A recorded NPC with no fate level that wears an item adopts its "
          "record as its roll; one wearing nothing is rolled beside it",
          "[NpcFate][espm]")
{
  constexpr uint32_t kWorn = 0x39fe2;
  constexpr uint32_t kBare = 0x39fe4;
  constexpr uint32_t kBarrow = 0x371de;
  constexpr uint32_t kPlayer = 0xff000d51;
  constexpr uint32_t kGold = 0xf;
  constexpr uint32_t kIronDagger = 0x1397e;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& worn = p.worldState.GetFormAt<MpActor>(kWorn);
  auto& bare = p.worldState.GetFormAt<MpActor>(kBare);
  REQUIRE(worn.CanRollFate());
  REQUIRE(bare.CanRollFate());

  // the records, as a save from before ADR-049 holds them: no fateLevel
  MpChangeForm wornRecord = worn.GetChangeForm();
  wornRecord.inv = Inventory();
  wornRecord.inv.AddItems(
    { Inventory::Entry(kIronDagger, 1), Inventory::Entry(kGold, 10) });
  wornRecord.equipment = Equipment();
  wornRecord.equipment.inv.entries.push_back(
    Inventory::Entry(kIronDagger, 1, kWornRight));
  wornRecord.fateLevel.reset();
  worn.ApplyChangeForm(wornRecord);
  MpChangeForm bareRecord = bare.GetChangeForm();
  bareRecord.inv = Inventory();
  bareRecord.inv.AddItems({ Inventory::Entry(kGold, 10) });
  bareRecord.equipment = Equipment();
  bareRecord.fateLevel.reset();
  bare.ApplyChangeForm(bareRecord);
  REQUIRE(worn.CanRollFate());
  REQUIRE(bare.CanRollFate());

  DoConnect(p, 0);
  p.CreateActor(kPlayer, { -3400.f, 700.f, 583.f }, 0, kBarrow);
  p.SetUserActor(0, kPlayer);
  auto& player = p.worldState.GetFormAt<MpActor>(kPlayer);
  ActorValueRecord level;
  level.level = 1;
  player.SetActorValueRecord(level);
  p.Messages().clear();
  p.TickCellHosts(FateNowMs());

  // adopted: the level set and nothing rolled, the record exactly as it was:
  // its two rows, the gold it held unchanged, the dagger its one worn entry
  REQUIRE(worn.GetFateLevel() == std::optional<uint16_t>{ 1 });
  REQUIRE_FALSE(worn.CanRollFate());
  REQUIRE(worn.GetInventory().GetItemCount(kIronDagger) == 1);
  REQUIRE(worn.GetInventory().GetItemCount(kGold) == 10);
  REQUIRE(worn.GetInventory().entries.size() == 2);
  REQUIRE(worn.GetEquipment().inv.entries.size() == 1);
  REQUIRE(Wearing(worn, kIronDagger));

  // rolled beside, as before D-N1 A: the level set, the record's rows kept
  // and the roll's added (its own gold may join the record's: pipeline 5691
  // read 17 and the mirror's Windows run 18, so the record's 10 is a floor)
  REQUIRE(bare.GetFateLevel() == std::optional<uint16_t>{ 1 });
  REQUIRE(bare.GetInventory().GetItemCount(kGold) >= 10);
  REQUIRE(bare.GetInventory().entries.size() > 1);

  // the adopted outfit rides its fate
  bool sentWorn = false;
  for (const auto& msg : SentTo<ActorFateMessage>(p, 0)) {
    for (const auto& f : msg.fates) {
      if (f.refrId == kWorn) {
        sentWorn =
          std::find(f.worn.begin(), f.worn.end(), kIronDagger) != f.worn.end();
      }
    }
  }
  REQUIRE(sentWorn);
  DoDisconnect(p, 0);
  p.DestroyActor(kPlayer);
}

// thuum playtest 50 (Embershard's undressed bandits; b6fec's roll: arrows
// +19 and 3 pieces missing): a recorded bandit's re-roll adds in form id
// order, and Inventory::AddItems returned at the first item that merged into
// a stack the record held, so everything the roll laid after it was lost
// while the worn set already named the roll's outfit, and the fate named only
// the worn pieces the inventory held. The campfire bandit above, holding 10
// gold (which its roll never yields) and one iron arrow (which it does, one
// to twenty): the arrows 0x1397d merge, and the dagger 0x1397e, the bow and
// any outfit piece above them still land, the fate naming all it wears.
TEST_CASE("A bandit's re-roll onto a stack it holds lands whole and its fate "
          "names everything it wears",
          "[NpcFate][espm]")
{
  constexpr uint32_t kBandit = 0x39fe2;
  constexpr uint32_t kBarrow = 0x371de;
  constexpr uint32_t kPlayer = 0xff000d41;
  constexpr uint32_t kGold = 0xf;
  constexpr uint32_t kIronDagger = 0x1397e;
  constexpr uint32_t kIronArrow = 0x1397d;
  constexpr uint32_t kHuntingBow = 0x13985;
  constexpr uint32_t kLongBow = 0x3b562;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& bandit = p.worldState.GetFormAt<MpActor>(kBandit);
  REQUIRE(p.worldState.CellOf(bandit) == kBarrow);
  bandit.AddItem(kGold, 10);
  bandit.AddItem(kIronArrow, 1);
  REQUIRE(bandit.CanRollFate());

  DoConnect(p, 0);
  p.CreateActor(kPlayer, { -3400.f, 700.f, 583.f }, 0, kBarrow);
  p.SetUserActor(0, kPlayer);
  auto& player = p.worldState.GetFormAt<MpActor>(kPlayer);
  ActorValueRecord record;
  record.level = 1;
  player.SetActorValueRecord(record);
  p.Messages().clear();
  p.TickCellHosts(FateNowMs());

  REQUIRE(bandit.GetFateLevel() == std::optional<uint16_t>{ 1 });
  const Inventory inv = bandit.GetInventory();
  INFO("gold " << inv.GetItemCount(kGold) << " dagger "
               << inv.GetItemCount(kIronDagger) << " arrows "
               << inv.GetItemCount(kIronArrow) << " hunting "
               << inv.GetItemCount(kHuntingBow) << " long "
               << inv.GetItemCount(kLongBow));
  REQUIRE(inv.GetItemCount(kGold) == 10);
  REQUIRE(inv.GetItemCount(kIronArrow) >= 2);
  REQUIRE(inv.GetItemCount(kIronArrow) <= 21);
  REQUIRE(inv.GetItemCount(kIronDagger) == 1);
  REQUIRE(inv.GetItemCount(kHuntingBow) + inv.GetItemCount(kLongBow) == 1);
  REQUIRE((Wearing(bandit, kHuntingBow) || Wearing(bandit, kLongBow)));

  const auto fates = SentTo<ActorFateMessage>(p, 0);
  const ActorFateMessage::Fate* sent = nullptr;
  for (const auto& msg : fates) {
    for (const auto& f : msg.fates) {
      if (f.refrId == kBandit) {
        sent = &f;
      }
    }
  }
  REQUIRE(sent != nullptr);
  // every piece the record wears (the roll's outfit, the bow in hand) is in
  // its inventory and named by the fate
  size_t wornCount = 0;
  for (const auto& entry : bandit.GetEquipment().inv.entries) {
    if (entry.GetWorn() == Inventory::Worn::None) {
      continue;
    }
    ++wornCount;
    INFO("worn " << std::hex << entry.baseId);
    REQUIRE(inv.GetItemCount(entry.baseId) >= 1);
    REQUIRE(std::find(sent->worn.begin(), sent->worn.end(), entry.baseId) !=
            sent->worn.end());
  }
  REQUIRE(sent->worn.size() == wornCount);
  // and the rows carry the rest of the roll beside the put
  auto rowOf = [&](uint32_t baseId) -> uint32_t {
    for (const auto& r : sent->inv) {
      if (r.baseId == baseId) {
        return r.count;
      }
    }
    return 0;
  };
  REQUIRE(rowOf(kGold) == 10);
  REQUIRE(rowOf(kIronDagger) == 1);
  REQUIRE(rowOf(kIronArrow) == inv.GetItemCount(kIronArrow));

  DoDisconnect(p, 0);
  p.DestroyActor(kPlayer);
}

// thuum playtest 51 (b-npc-stream-equipment, run 20261110-062127): ten
// Riverwood residents the game files place indoors reached their host with
// no roll. Since fork c312fd39 (2026-11-03) Init leaves a fate-able actor's
// roll to its fate, and only the pass's walk and PlaceAtMe rolled: c1's pass
// at (4,-11) fated 32 actors and none of the ten (their records stood in
// their houses), then c1's npc-puppet sighting moved each outdoors into a
// square that pass had fated, so no pass came back for it, and c1's first
// equipment report met an empty record: every worn piece failed the
// inventory check and the hosted refusal dropped the report whole
// (E_EQUIP_REFUSED). Faendal 0x1348c (base 0x13480, its outfit 0xdab76 of
// 0x209a5 and 0x6c1d9) stands for the ten, his record in
// RiverwoodFaendalsHouse 0x133ca; the 11-02 build, which rolled at Init,
// took the same report ("3 worn, right 209a5 left 13985"). The roll now
// runs at his first create word (EnsureFated), at c1's level.
TEST_CASE("An indoor resident the host's sighting moves into a square its "
          "pass fated rolls at its create word, its fate ahead of the word, "
          "and the host's first equipment report is taken",
          "[NpcFate][HostedEquipment][espm]")
{
  enum : uint32_t
  {
    kHost = 0xff000d50,
    kFaendal = 0x1348c,
    kFaendalsHouse = 0x133ca,
    // Skyrim.esm: the outfit's two pieces, and the HuntingBow his host's
    // engine had in hand
    kOutfitA = 0x6c1d9,
    kOutfitB = 0x209a5,
    kHuntingBow = 0x13985
  };
  const NiPoint3 kLanding{ 17225.f, -47204.f, -128.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;

  // c1 lands at Riverwood with its level, its pass fates the ring around
  // (4,-11) and it hosts the landing's cell
  DoConnect(p, 0);
  p.CreateActor(kHost, kLanding, 0, kTamriel);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  ActorValueRecord record;
  record.level = 9;
  host.SetActorValueRecord(record);
  p.SetUserActor(0, kHost);
  const auto cell = p.worldState.CellOf(kTamriel, kLanding);
  REQUIRE(cell.has_value());
  HostGrants(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kHost);

  // the record the pass never reached: indoors, loaded as the sighting
  // loads it (here ahead of the sighting), served to no game, no roll yet
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kFaendal) == nullptr);
  const auto& form = p.worldState.LookupFormById(kFaendal, nullptr, true);
  REQUIRE(form);
  REQUIRE(form->AsActor());
  MpActor& faendal = *form->AsActor();
  REQUIRE(p.worldState.IsInteriorCell(kFaendalsHouse));
  REQUIRE(p.worldState.CellOf(faendal) ==
          std::optional<uint32_t>(kFaendalsHouse));
  REQUIRE(faendal.CanRollFate());
  REQUIRE(faendal.GetInventory().GetItemCount(kOutfitA) == 0);
  REQUIRE(faendal.GetInventory().GetItemCount(kOutfitB) == 0);

  // c1's roster sights him outdoors at the landing, in the cell c1 hosts
  // and the square its pass fated: the record moves there and reaches c1
  p.Messages().clear();
  p.OnActorSighting(host, kFaendal, kTamriel,
                    kLanding + NiPoint3{ 150.f, 0.f, 0.f },
                    NiPoint3{ 0.f, 0.f, 90.f });
  REQUIRE(p.worldState.CellOf(faendal) == cell);
  REQUIRE(p.worldState.HostOf(faendal) == &host);

  // rolled once, at c1's level (its pass's own); the outfit held and worn
  REQUIRE_FALSE(faendal.CanRollFate());
  REQUIRE(faendal.GetFateLevel() == std::optional<uint16_t>(9));
  REQUIRE(faendal.GetInventory().GetItemCount(kOutfitA) >= 1);
  REQUIRE(faendal.GetInventory().GetItemCount(kOutfitB) >= 1);
  REQUIRE(Wearing(faendal, kOutfitA));
  REQUIRE(Wearing(faendal, kOutfitB));

  // c1 hears his fate once, ahead of his create word (its pass fated the
  // square before he came, and comes back for nothing), and both carry the
  // outfit
  REQUIRE(FatesOf(p, 0, kFaendal) == 1);
  const auto fateAt = FirstFateAt(p, 0, kFaendal);
  const auto createAt = FirstCreateAt(p, 0, kFaendal);
  REQUIRE(fateAt.has_value());
  REQUIRE(createAt.has_value());
  REQUIRE(*fateAt < *createAt);
  // and nothing else of his reaches c1 ahead of that word: the roll's
  // equipment word went to the listeners with a user he had (none), not
  // through his own listening to his cell's host, c1 (the review of
  // 062e9065, note 1)
  REQUIRE(IdxWordsBefore(p, 0, faendal.GetIdx(), *createAt) == 0);
  for (size_t i = 0; i < *createAt; ++i) {
    auto word =
      dynamic_cast<UpdateEquipmentMessage*>(p.Messages()[i].message.get());
    REQUIRE_FALSE((word && word->idx == faendal.GetIdx()));
  }
  {
    auto fate =
      dynamic_cast<ActorFateMessage*>(p.Messages()[*fateAt].message.get());
    REQUIRE(fate);
    const auto sent = std::find_if(
      fate->fates.begin(), fate->fates.end(),
      [](const ActorFateMessage::Fate& f) { return f.refrId == kFaendal; });
    REQUIRE(sent != fate->fates.end());
    REQUIRE(sent->level == 9);
    const auto names = [&](uint32_t baseId) {
      return std::find(sent->worn.begin(), sent->worn.end(), baseId) !=
        sent->worn.end();
    };
    REQUIRE(names(kOutfitA));
    REQUIRE(names(kOutfitB));
    auto create =
      dynamic_cast<CreateActorMessage*>(p.Messages()[*createAt].message.get());
    REQUIRE(create);
    REQUIRE(create->equipment.has_value());
    REQUIRE(create->equipment->inv.HasItem(kOutfitA));
    REQUIRE(create->equipment->inv.HasItem(kOutfitB));
  }

  // c1's first equipment report as 062127 logged it: taken whole
  {
    LogCapture log;
    Wears(p, faendal, { kOutfitA, kOutfitB, kHuntingBow }, 7);
    REQUIRE(log.Count("E_EQUIP_REFUSED") == 0);
    REQUIRE(log.Count("inventory does not contain item") == 0);
  }
  REQUIRE(faendal.GetEquipment().numChanges == 7);
  REQUIRE(Wearing(faendal, kOutfitA));
  REQUIRE(Wearing(faendal, kOutfitB));
  REQUIRE(Wearing(faendal, kHuntingBow));
  REQUIRE(faendal.GetInventory().HasItem(kHuntingBow));

  DoDisconnect(p, 0);
  p.DestroyActor(kHost);
}

// thuum playtest 51 (b-npc-stream-equipment): the sighting is one late path
// that moves a record no game was served into a square a pass has fated; a
// script's MoveTo is another (PapyrusObjectReference::MoveTo, an actor's
// Teleport: the sighting's three sets behind a teleport word), and a
// lookup's or an alias's load the same. Sigrid 0x13483, placed in Alvor's
// house (CellHostTest's resident), is moved to c1's landing: she rolls once,
// at her first create word, c1 hearing her fate ahead of it, and c2, two
// squares east (its pass fated her square, its 3x3 is short of her), hears
// the same fate then with no create word (the review of 062e9065, note 8);
// her next create word to c1 rolls nothing and carries the same fate again,
// and c2 hears nothing more.
TEST_CASE("A script's MoveTo of a resident nothing rolled into a square a "
          "pass fated rolls her once at her create word, her fate ahead of "
          "every create word and to a game beyond her 3x3 whose pass fated "
          "the square",
          "[NpcFate][espm]")
{
  enum : uint32_t
  {
    kFirst = 0xff000d51,
    kSecond = 0xff000d52,
    kSigrid = 0x13483
  };
  const NiPoint3 kLanding{ 17225.f, -47204.f, -128.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  DoConnect(p, 0);
  p.CreateActor(kFirst, kLanding, 0, kTamriel);
  auto& first = p.worldState.GetFormAt<MpActor>(kFirst);
  ActorValueRecord record;
  record.level = 14;
  first.SetActorValueRecord(record);
  p.SetUserActor(0, kFirst);
  // c2 has no record, so the roll's level is c1's alone (the party's mean)
  DoConnect(p, 1);
  p.CreateActor(kSecond, kLanding + NiPoint3{ 2 * 4096.f, 0.f, 0.f }, 0,
                kTamriel);
  auto& second = p.worldState.GetFormAt<MpActor>(kSecond);
  p.SetUserActor(1, kSecond);
  p.TickCellHosts(FateNowMs());

  // loaded where the files place her, as a host's sighting loads her
  // (CellHostTest's), served to no game
  const auto& form = p.worldState.LookupFormById(kSigrid, nullptr, true);
  REQUIRE(form);
  REQUIRE(form->AsActor());
  MpActor& sigrid = *form->AsActor();
  const auto home = p.worldState.CellOf(sigrid);
  REQUIRE(home.has_value());
  REQUIRE(p.worldState.IsInteriorCell(*home));
  REQUIRE(sigrid.CanRollFate());
  const auto moveTo = [&](const NiPoint3& pos) {
    sigrid.Teleport(
      LocationalData{ pos, { 0.f, 0.f, 0.f }, FormDesc::Tamriel() });
  };

  // the MoveTo: her first create word, to c1, carries her roll at c1's
  // level with her fate ahead of it; c2 hears the fate alone
  p.Messages().clear();
  moveTo(kLanding + NiPoint3{ 150.f, 0.f, 0.f });
  REQUIRE(sigrid.GetListeners().count(&first) == 1);
  REQUIRE(sigrid.GetListeners().count(&second) == 0);
  REQUIRE_FALSE(sigrid.CanRollFate());
  REQUIRE(sigrid.GetFateLevel() == std::optional<uint16_t>(14));
  REQUIRE(sigrid.GetChangeForm().baseContainerAdded);
  REQUIRE(FatesOf(p, 0, kSigrid) == 1);
  const auto fateAt = FirstFateAt(p, 0, kSigrid);
  const auto createAt = FirstCreateAt(p, 0, kSigrid);
  REQUIRE(fateAt.has_value());
  REQUIRE(createAt.has_value());
  REQUIRE(*fateAt < *createAt);
  REQUIRE(IdxWordsBefore(p, 0, sigrid.GetIdx(), *createAt) == 0);
  REQUIRE(FatesOf(p, 1, kSigrid) == 1);
  REQUIRE_FALSE(FirstCreateAt(p, 1, kSigrid).has_value());
  const auto fated = FirstFateOf(p, 0, kSigrid);
  REQUIRE(fated.has_value());
  REQUIRE(FirstFateOf(p, 1, kSigrid) == fated);
  {
    // the word carries the record's equipment, and what she wears she holds
    auto create =
      dynamic_cast<CreateActorMessage*>(p.Messages()[*createAt].message.get());
    REQUIRE(create);
    REQUIRE(create->equipment.has_value());
    REQUIRE(create->equipment->inv.entries.size() ==
            sigrid.GetEquipment().inv.entries.size());
    for (const auto& entry : sigrid.GetEquipment().inv.entries) {
      if (entry.GetWorn() != Inventory::Worn::None) {
        INFO("worn " << std::hex << entry.baseId);
        REQUIRE(sigrid.GetInventory().GetItemCount(entry.baseId) >= 1);
      }
    }
  }
  const std::string rolled = sigrid.GetInventory().ToJson().dump();

  // out of both games' reach, west, and back to c1: a second create word,
  // no roll, the same fate ahead of it; c2 hears nothing
  moveTo(kLanding - NiPoint3{ 3 * 4096.f, 0.f, 0.f });
  REQUIRE(sigrid.GetListeners().count(&first) == 0);
  p.Messages().clear();
  moveTo(kLanding + NiPoint3{ 200.f, 0.f, 0.f });
  REQUIRE(sigrid.GetListeners().count(&first) == 1);
  REQUIRE(FatesOf(p, 0, kSigrid) == 1);
  const auto againAt = FirstFateAt(p, 0, kSigrid);
  const auto recreatedAt = FirstCreateAt(p, 0, kSigrid);
  REQUIRE(againAt.has_value());
  REQUIRE(recreatedAt.has_value());
  REQUIRE(*againAt < *recreatedAt);
  REQUIRE(FirstFateOf(p, 0, kSigrid) == fated);
  REQUIRE(FatesOf(p, 1, kSigrid) == 0);
  REQUIRE(sigrid.GetFateLevel() == std::optional<uint16_t>(14));
  REQUIRE(sigrid.GetInventory().ToJson().dump() == rolled);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kFirst);
  p.DestroyActor(kSecond);
}

// thuum playtest 51: the pass's walk is the roll's other first reach. An
// actor the grid loaded where no game had it in reach (here a lookup's
// load; another actor's square change loads a 3x3 the same) is served to
// no game until a pass reaches its square: the walk rolls it there, at the
// walker's level, and its batch sends the fate once; the create word that
// follows when the walker comes up rolls nothing more and carries the same
// fate ahead of it.
TEST_CASE("An actor the pass's walk rolled is not rolled again at its create "
          "word, which carries the walk's fate ahead of it",
          "[NpcFate][espm]")
{
  constexpr uint32_t kWalker = 0xff000d53;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = p.worldState.GetFormAt<MpActor>(kPredator);
  REQUIRE(predator.CanRollFate());

  // the walker lands two squares east: inside its pass's ring, outside the
  // grid's 3x3, so its login serves the predator nothing
  DoConnect(p, 0);
  p.CreateActor(kWalker, kForest + NiPoint3{ 2 * 4096.f, 0.f, 0.f }, 0,
                kTamriel);
  auto& walker = p.worldState.GetFormAt<MpActor>(kWalker);
  ActorValueRecord record;
  record.level = 17;
  walker.SetActorValueRecord(record);
  p.SetUserActor(0, kWalker);
  REQUIRE(predator.GetListeners().count(&walker) == 0);
  REQUIRE(predator.CanRollFate());

  // the pass: the walk rolls it at the walker's level and sends its fate
  p.Messages().clear();
  p.TickCellHosts(FateNowMs());
  REQUIRE_FALSE(predator.CanRollFate());
  REQUIRE(predator.GetFateLevel() == std::optional<uint16_t>(17));
  REQUIRE(predator.GetListeners().count(&walker) == 0);
  REQUIRE(FatesOf(p, 0, kPredator) == 1);
  const auto walked = FirstFateOf(p, 0, kPredator);
  REQUIRE(walked.has_value());
  const std::string rolled = predator.GetInventory().ToJson().dump();

  // the walker comes up: its create word rolls nothing and carries the
  // walk's fate ahead of it
  p.Messages().clear();
  walker.SetPos(kForest);
  REQUIRE(predator.GetListeners().count(&walker) == 1);
  REQUIRE(FatesOf(p, 0, kPredator) == 1);
  const auto fateAt = FirstFateAt(p, 0, kPredator);
  const auto createAt = FirstCreateAt(p, 0, kPredator);
  REQUIRE(fateAt.has_value());
  REQUIRE(createAt.has_value());
  REQUIRE(*fateAt < *createAt);
  REQUIRE(FirstFateOf(p, 0, kPredator) == walked);
  REQUIRE(predator.GetFateLevel() == std::optional<uint16_t>(17));
  REQUIRE(predator.GetInventory().ToJson().dump() == rolled);

  DoDisconnect(p, 0);
  p.DestroyActor(kWalker);
}

// thuum playtest 51 (the review of 062e9065, notes 2 and 8): a login's
// create words for the actors in its 3x3 are each its actor's first
// serve, so each carries a roll made at the login's level, with the
// actor's fate ahead of it, and nothing else about the actor (an equipment
// word, a property) reaches the game ahead of that word. The login's first
// pass then sends the fates again, which the game takes for free while
// they are the same.
TEST_CASE("A login's create words carry the rolls of the actors in its 3x3, "
          "each behind its actor's fate and nothing else of it",
          "[NpcFate][espm]")
{
  constexpr uint32_t kGuest = 0xff000d54;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  DoConnect(p, 0);
  p.CreateActor(kGuest, kForest, 0, kTamriel);
  auto& guest = p.worldState.GetFormAt<MpActor>(kGuest);
  ActorValueRecord record;
  record.level = 21;
  guest.SetActorValueRecord(record);
  p.Messages().clear();
  p.SetUserActor(0, kGuest);

  size_t served = 0;
  bool predatorServed = false;
  auto& messages = p.Messages();
  for (size_t i = 0; i < messages.size(); ++i) {
    auto create = dynamic_cast<CreateActorMessage*>(messages[i].message.get());
    if (messages[i].userId != 0 || !create || !create->refrId ||
        *create->refrId >= 0xff000000) {
      continue;
    }
    const auto refrId = static_cast<uint32_t>(*create->refrId);
    const auto& form = p.worldState.LookupFormByIdNoLoad(refrId);
    MpActor* actor = form ? form->AsActor() : nullptr;
    // a reference that is no actor, or a body (it rolls at its first open)
    if (!actor || actor->IsDead()) {
      continue;
    }
    INFO("actor " << std::hex << refrId);
    REQUIRE(actor->GetFateLevel() == std::optional<uint16_t>(21));
    const auto fateAt = FirstFateAt(p, 0, refrId);
    REQUIRE(fateAt.has_value());
    REQUIRE(*fateAt < i);
    REQUIRE(IdxWordsBefore(p, 0, actor->GetIdx(), i) == 0);
    predatorServed = predatorServed || refrId == kPredator;
    ++served;
  }
  REQUIRE(served > 0);
  REQUIRE(predatorServed);

  DoDisconnect(p, 0);
  p.DestroyActor(kGuest);
}
