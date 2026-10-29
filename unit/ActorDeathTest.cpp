#include "ActionListener.h"
#include "ActivateMessage.h"
#include "ActorDeathMessage.h"
#include "CellLoadedMessage.h"
#include "ChangeValuesMessage.h"
#include "DamageCauseMessage.h"
#include "HitMessage.h"
#include "HostedActorStateMessage.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include "libespm/Loader.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>

PartOne& GetPartOne();

// thuum docs/verbs/npc-fate.md (ADR-048's B): a host's death report for a
// game-file actor it simulates is taken when the server can check its cause
// against what it saw, and refused otherwise. The forest's leveled
// predators (NpcFateTest's ids) stand in Tamriel's cell 0x9810. The shared
// world is left as found: a body stood up again, no deadline behind, the
// player gone (the fixture persists actors across cases).

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kHost = 0xff000d06;
constexpr uint32_t kOther = 0xff000d07;
constexpr uint32_t kPredator = 0x85547;
constexpr uint32_t kPredator2 = 0x85549;
const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// `user` arrives at the forest as `id`; the fate pass loads the ring
MpActor& Arrive(PartOne& p, Networking::UserId user, uint32_t id)
{
  DoConnect(p, user);
  p.CreateActor(id, kForest, 0, kTamriel);
  p.SetUserActor(user, id);
  p.TickCellHosts(NowMs());
  return p.worldState.GetFormAt<MpActor>(id);
}

// the predator, alive and with no deadline, as the earlier cases may have
// left it
MpActor& Standing(PartOne& p, uint32_t refrId)
{
  auto& form = p.worldState.LookupFormByIdNoLoad(refrId);
  REQUIRE(form);
  MpActor* actor = form->AsActor();
  REQUIRE(actor);
  if (actor->IsDead()) {
    actor->Respawn(false);
  }
  p.worldState.fateRespawnDue.erase(refrId);
  return *actor;
}

// `user` hosts `cell`: any earlier host left at its case's end, so its grace
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

void Report(PartOne& p, Networking::UserId user, uint32_t refrId,
            uint32_t cause, uint32_t killer, const NiPoint3& pos)
{
  ActorDeathMessage msg;
  msg.refrId = refrId;
  msg.cause = cause;
  msg.killer = killer;
  msg.pos = { pos.x, pos.y, pos.z };
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnActorDeath(raw, msg);
}

// a reference's activation of itself, the trap trigger's firing as the
// host's game reports it (activationService): caster and target the same
void SelfActivation(PartOne& p, Networking::UserId user, uint32_t refId)
{
  ActivateMessage msg;
  msg.data.caster = refId;
  msg.data.target = refId;
  msg.data.isSecondActivation = false;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnActivate(raw, msg);
}

void Leave(PartOne& p, Networking::UserId user, uint32_t id)
{
  DoDisconnect(p, user);
  p.DestroyActor(id);
}

void StandUp(PartOne& p, MpActor& actor)
{
  if (actor.IsDead()) {
    actor.Respawn(false);
  }
  p.worldState.fateRespawnDue.erase(actor.GetFormId());
}

// `user`'s state sample of `actorId` (CellHostTest's State idiom): the
// percentages, no target and no package
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
}

// thuum docs/verbs/npc-fate.md (playtest 37's finding 48, 2026-10-29): a
// necromancer's Reanimate stood a dead skeever up on its host's engine, where
// it fought, while the record and the other seat kept the body
TEST_CASE("A hosted NPC the record holds dead whose host samples it alive "
          "stands again by its host's engine's word: after the grace of its "
          "death, from its host alone, with the live word to the other game",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  auto& host = Arrive(p, 0, kHost);
  Arrive(p, 1, kOther);
  auto& actor = Standing(p, kPredator);
  actor.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &host);
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  const auto now = std::chrono::steady_clock::now();

  // a live sample within the grace of the death: the host's sample in flight
  // when the server's word landed, not a resurrection
  State(p, 0, kPredator, 1.f);
  REQUIRE(actor.IsDead());

  // past the grace, from the other game: dropped at the host gate
  actor.RecordDeathAt(
    now - std::chrono::milliseconds(MpActor::kPendingDeathMs + 1000));
  State(p, 1, kPredator, 1.f);
  REQUIRE(actor.IsDead());

  // from the host: the death cleared, the body's day and its due ended, the
  // live word to the other game
  p.Messages().clear();
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
  REQUIRE_FALSE(actor.GetChangeForm().fateDiedAt.has_value());
  REQUIRE(p.worldState.fateRespawnDue.count(kPredator) == 0);
  bool liveWord = false;
  for (auto& m : p.Messages()) {
    if (m.userId == 1 && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::UpdateProperty) &&
        m.j.value("propName", std::string()) == "isDead" &&
        m.j.value("refrId", 0u) == kPredator &&
        m.j.value("dataDump", std::string()) == "false") {
      liveWord = true;
    }
  }
  REQUIRE(liveWord);

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("A host's fall report is taken when its own samples show the drop "
          "and refused when they do not",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  Host(p, 0, *cell);
  REQUIRE(p.SimulatorOf(actor) == kHost);
  const NiPoint3 body = actor.GetPos();

  // no samples: nothing to check the fall against
  Report(p, 0, kPredator, 1, 0, body);
  REQUIRE_FALSE(actor.IsDead());

  // the host's samples: a ledge 500 units up two seconds ago, the ground now
  const auto now = std::chrono::steady_clock::now();
  actor.RecordHeight(body.z + 500.f, now - std::chrono::seconds(2));
  actor.RecordHeight(body.z, now);
  Report(p, 0, kPredator, 1, 0, body);
  REQUIRE(actor.IsDead());

  StandUp(p, actor);
  Leave(p, 0, kHost);
}

TEST_CASE("A death report from a game that does not simulate the actor is "
          "refused",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  Host(p, 0, *cell);
  // a second player in the same forest, hosting nothing
  Arrive(p, 1, kOther);
  REQUIRE(p.SimulatorOf(actor) == kHost);
  const NiPoint3 body = actor.GetPos();
  const auto now = std::chrono::steady_clock::now();
  actor.RecordHeight(body.z + 500.f, now - std::chrono::seconds(2));
  actor.RecordHeight(body.z, now);

  Report(p, 1, kPredator, 1, 0, body);
  REQUIRE_FALSE(actor.IsDead());
  // the host's own word stands
  Report(p, 0, kPredator, 1, 0, body);
  REQUIRE(actor.IsDead());

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("A report naming an actor's hit is checked against the hits the "
          "server numbered",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  auto& killer = Standing(p, kPredator2);
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  Host(p, 0, *cell);
  REQUIRE(p.SimulatorOf(actor) == kHost);
  const NiPoint3 body = actor.GetPos();

  // no hit seen by that killer: refused
  Report(p, 0, kPredator, 4, kPredator2, body);
  REQUIRE_FALSE(actor.IsDead());
  // a hit by another actor is not the one named
  actor.NoteHitBy(kHost, std::chrono::steady_clock::now());
  Report(p, 0, kPredator, 4, kPredator2, body);
  REQUIRE_FALSE(actor.IsDead());
  // the server numbered the killer's hit a moment ago: the engine's number
  // outran the server's, the death stands
  actor.NoteHitBy(killer.GetFormId(), std::chrono::steady_clock::now());
  Report(p, 0, kPredator, 4, kPredator2, body);
  REQUIRE(actor.IsDead());

  StandUp(p, actor);
  Leave(p, 0, kHost);
}

TEST_CASE("A report naming no killer after a numbered hit credits the "
          "hit's aggressor from the server's own number (npc-fate H-52b)",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  auto& striker = Standing(p, kPredator2);
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  Host(p, 0, *cell);
  REQUIRE(p.SimulatorOf(actor) == kHost);
  const NiPoint3 body = actor.GetPos();

  // the server numbered the striker's hit a moment ago and the host's
  // engine named no killer (a lethal single blow, cause 0, 52's gate's rest
  // 153601 and 160510): the death stands and the striker is credited
  actor.NoteHitBy(striker.GetFormId(), std::chrono::steady_clock::now());
  Report(p, 0, kPredator, 0, 0, body);
  REQUIRE(actor.IsDead());
  REQUIRE(actor.LastKillerId().has_value());
  REQUIRE(*actor.LastKillerId() == striker.GetFormId());

  StandUp(p, actor);
  Leave(p, 0, kHost);
}

TEST_CASE("A hazard report is checked against an activation beside the body",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  Host(p, 0, *cell);
  REQUIRE(p.SimulatorOf(actor) == kHost);
  const NiPoint3 body = actor.GetPos();

  Report(p, 0, kPredator, 2, 0, body);
  REQUIRE_FALSE(actor.IsDead());
  // a trigger's own firing reported by a game that does not host the cell
  // is nothing (the reference stands in for a plate: the path is the same)
  Arrive(p, 1, kOther);
  SelfActivation(p, 1, kPredator);
  Report(p, 0, kPredator, 2, 0, body);
  REQUIRE_FALSE(actor.IsDead());
  // the host's report of the trigger firing beside the body a moment ago
  SelfActivation(p, 0, kPredator);
  Report(p, 0, kPredator, 2, 0, body);
  REQUIRE(actor.IsDead());

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("A script's kill, an unnamed cause and a body far from the record "
          "are refused, a dead actor's report is nothing, and the setting "
          "off refuses everything",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  Host(p, 0, *cell);
  REQUIRE(p.SimulatorOf(actor) == kHost);
  const NiPoint3 body = actor.GetPos();
  const auto now = std::chrono::steady_clock::now();
  actor.RecordHeight(body.z + 500.f, now - std::chrono::seconds(2));
  actor.RecordHeight(body.z, now);

  Report(p, 0, kPredator, 3, 0, body);
  REQUIRE_FALSE(actor.IsDead());
  Report(p, 0, kPredator, 7, 0, body);
  REQUIRE_FALSE(actor.IsDead());
  // the drop would pass, but the body lies 3000 units away
  Report(p, 0, kPredator, 1, 0, body + NiPoint3{ 3000.f, 0.f, 0.f });
  REQUIRE_FALSE(actor.IsDead());
  // the setting off: ADR-048's A, the server's number alone
  p.worldState.npcDeathReports = false;
  Report(p, 0, kPredator, 1, 0, body);
  REQUIRE_FALSE(actor.IsDead());
  p.worldState.npcDeathReports = true;
  // the unknown cause with the drop as its evidence
  Report(p, 0, kPredator, 0, 0, body);
  REQUIRE(actor.IsDead());
  // a report for a body already dead changes nothing and throws nothing
  Report(p, 0, kPredator, 1, 0, body);
  REQUIRE(actor.IsDead());

  StandUp(p, actor);
  Leave(p, 0, kHost);
}

// thuum docs/verbs/npc-fate.md (playtest 34, section 5): a body is a
// container with one occupant within 512 units, and its looter's closing
// activation must release it. ProcessActivateSecond shared the CONT branch's
// open but not its close, so rotfern's closing activation fell through, the
// body stayed hers, and test 2 was refused five times ("occupant is another
// object and is nearby") until she stood out of range.
TEST_CASE("A looter's closing activation releases a corpse's occupancy for "
          "the next player",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& host = Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  const NiPoint3 body = actor.GetPos();
  host.SetPos(body + NiPoint3{ 60.f, 0.f, 0.f });
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());

  const auto activate = [&](Networking::UserId user, bool second) {
    DoMessage(p, user,
              nlohmann::json{ { "t", MsgType::Activate },
                              { "data",
                                { { "caster", 0x14 },
                                  { "target", kPredator },
                                  { "isSecondActivation", second } } } });
  };

  // the looter opens the body: the CONT branch's open
  activate(0, false);
  REQUIRE(actor.IsOpen());
  // the closing activation releases it
  activate(0, true);
  REQUIRE_FALSE(actor.IsOpen());

  // a second player at the body is admitted at once, the looter still beside
  // it: the record's inventory and OpenContainer go to that player
  auto& other = Arrive(p, 1, kOther);
  other.SetPos(body + NiPoint3{ -60.f, 0.f, 0.f });
  p.Messages().clear();
  activate(1, false);
  REQUIRE(actor.IsOpen());
  auto& messages = p.Messages();
  const bool opened =
    std::any_of(messages.begin(), messages.end(), [&](PartOne::Message& m) {
      return m.userId == 1 && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::OpenContainer);
    });
  REQUIRE(opened);
  activate(1, true);
  REQUIRE_FALSE(actor.IsOpen());

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

namespace {
// the typed messages `user` was sent, in order
template <class Message>
std::vector<Message> TypedSentTo(PartOne& p, Networking::UserId user)
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

// the isDead words `user` heard for `refrId`
size_t DeathWordsTo(PartOne& p, Networking::UserId user, uint32_t refrId)
{
  size_t n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::UpdateProperty) &&
        m.j.value("propName", std::string()) == "isDead" &&
        m.j.value("refrId", 0u) == refrId &&
        m.j.value("dataDump", std::string()) == "true") {
      ++n;
    }
  }
  return n;
}

// `user`'s bare-handed hit on `target`
void HitBy(PartOne& p, Networking::UserId user, uint32_t target)
{
  RawMessageData raw;
  raw.userId = user;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = target;
  hit.data.source = 0x1f4; // bare hands, Skyrim.esm's Unarmed (HitTest's)
  p.GetActionListener().OnHit(raw, hit);
}
}

// thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine)
TEST_CASE("A numbered hit on a hosted NPC reaches its host as a cause and "
          "the other listeners as values, and the kill at zero waits for "
          "the host's report, once",
          "[NpcFate][Death][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  p.worldState.npcEngineDeaths = true;
  auto& host = Arrive(p, 0, kHost);
  auto& other = Arrive(p, 1, kOther);
  Host(p, 0, 0x9810);
  auto& predator = Standing(p, kPredator);
  predator.SetPercentages(ActorValues{}, nullptr);
  REQUIRE(predator.HostedBy() == &host);
  REQUIRE_FALSE(predator.IsDead());

  // the other player's hit: the host hears a cause for the health and no
  // absolute health; the other player hears the resulting values
  p.Messages().clear();
  HitBy(p, 1, kPredator);
  const auto causes = TypedSentTo<DamageCauseMessage>(p, 0);
  REQUIRE_FALSE(causes.empty());
  REQUIRE(causes.back().idx == predator.GetIdx());
  REQUIRE(causes.back().attacker == other.GetFormId());
  REQUIRE(causes.back().health < 1.f);
  REQUIRE_FALSE(causes.back().killMove);
  for (const auto& values : TypedSentTo<ChangeValuesMessage>(p, 0)) {
    if (values.idx == predator.GetIdx()) {
      REQUIRE_FALSE(values.data.health.has_value());
    }
  }
  bool otherHeard = false;
  for (const auto& values : TypedSentTo<ChangeValuesMessage>(p, 1)) {
    if (values.idx == predator.GetIdx() && values.data.health.has_value()) {
      otherHeard = true;
    }
  }
  REQUIRE(otherHeard);

  // the server's number reaches zero: no kill yet, the cause says 0, the
  // record reads 0, the death pends
  p.Messages().clear();
  ActorValues zero;
  zero.healthPercentage = 0.f;
  predator.NetSetPercentages(
    zero, &other, std::vector<espm::ActorValue>{ espm::ActorValue::Health });
  REQUIRE_FALSE(predator.IsDead());
  REQUIRE(predator.HasPendingDeath());
  REQUIRE(predator.GetChangeForm().actorValues.healthPercentage == 0.f);
  const auto lethal = TypedSentTo<DamageCauseMessage>(p, 0);
  REQUIRE_FALSE(lethal.empty());
  REQUIRE(lethal.back().health == 0.f);

  // the host's report ends it: one death, one word to the other game, and
  // the window's tick adds nothing
  p.Messages().clear();
  Report(p, 0, kPredator, 4, kOther, predator.GetPos());
  REQUIRE(predator.IsDead());
  REQUIRE_FALSE(predator.HasPendingDeath());
  REQUIRE(DeathWordsTo(p, 1, kPredator) == 1);
  p.TickDeaths(NowMs() + MpActor::kPendingDeathMs + 2'000);
  REQUIRE(DeathWordsTo(p, 1, kPredator) == 1);

  StandUp(p, predator);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("A hosted NPC at zero with no report dies by the server's word "
          "when the window passes, and a headshot defers the same way",
          "[NpcFate][Death][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  p.worldState.npcEngineDeaths = true;
  auto& host = Arrive(p, 0, kHost);
  auto& other = Arrive(p, 1, kOther);
  Host(p, 0, 0x9810);
  auto& predator = Standing(p, kPredator);
  predator.SetPercentages(ActorValues{}, nullptr);
  REQUIRE(predator.HostedBy() == &host);

  p.Messages().clear();
  ActorValues zero;
  zero.healthPercentage = 0.f;
  predator.NetSetPercentages(
    zero, &other, std::vector<espm::ActorValue>{ espm::ActorValue::Health });
  REQUIRE(predator.HasPendingDeath());
  const auto since = predator.PendingDeathSinceMs();
  p.TickDeaths(since + 1'000);
  REQUIRE_FALSE(predator.IsDead());
  p.TickDeaths(since + MpActor::kPendingDeathMs + 1);
  REQUIRE(predator.IsDead());
  REQUIRE(DeathWordsTo(p, 1, kPredator) == 1);

  // a kill the server decided (a headshot's shape): a lethal cause with the
  // kill move, deferred the same way; the switch off kills at once
  StandUp(p, predator);
  predator.SetPercentages(ActorValues{}, nullptr);
  p.Messages().clear();
  predator.KillOrDefer(&other, true);
  REQUIRE_FALSE(predator.IsDead());
  REQUIRE(predator.HasPendingDeath());
  const auto causes = TypedSentTo<DamageCauseMessage>(p, 0);
  REQUIRE_FALSE(causes.empty());
  REQUIRE(causes.back().killMove);
  REQUIRE(causes.back().health == 0.f);
  p.TickDeaths(predator.PendingDeathSinceMs() + MpActor::kPendingDeathMs + 1);
  REQUIRE(predator.IsDead());

  StandUp(p, predator);
  predator.SetPercentages(ActorValues{}, nullptr);
  p.worldState.npcEngineDeaths = false;
  p.Messages().clear();
  predator.KillOrDefer(&other, false);
  REQUIRE(predator.IsDead());
  REQUIRE(TypedSentTo<DamageCauseMessage>(p, 0).empty());
  p.worldState.npcEngineDeaths = true;

  StandUp(p, predator);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}
