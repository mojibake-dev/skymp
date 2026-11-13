#include "ActionListener.h"
#include "ActivateMessage.h"
#include "ActorDeathMessage.h"
#include "CellLoadedMessage.h"
#include "ChangeValuesMessage.h"
#include "DamageCauseMessage.h"
#include "HitMessage.h"
#include "HostedActorStateMessage.h"
#include "MsgType.h"
#include "PropOwnerMessage.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
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

  // the host samples the body dead first: its stamp (round six, run
  // 20261103-100946: a host whose game never sampled the body dead stands
  // nothing with a live sample, whatever the death's own stamp; the
  // necromancer's host had the skeever dead on its engine before the spell)
  State(p, 0, kPredator, 0.f);
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

// numidium's reviews of rounds six and seven (2026-11-03): the host's
// dead-sample stamp names its WITNESS (the host's id and login generation)
// and its time. The cases below: the cast's rule survives it; a NEW host's
// first live sample stands nothing until it has sampled the body dead; a
// RELAUNCHED host (a clean quit, the Release gone while it was away, the
// same id back) must sample the body dead again; a host that WALKED OFF
// before its quit (the cell unhosted, no Release flip reaching the body) the
// same; the engine's death first (the host's zero on a still-alive record)
// is that death's stamp; a bleedout's zero long before a later death is not;
// the host's reliable death report is a stamp too

static void BackdatePastGrace(MpActor& actor)
{
  actor.RecordDeathAt(
    std::chrono::steady_clock::now() -
    std::chrono::milliseconds(MpActor::kPendingDeathMs + 1000));
}

static MpActor& HostedPredator(PartOne& p, MpActor*& hostOut, bool withOther)
{
  constexpr uint32_t kForestCell = 0x9810;
  auto& host = Arrive(p, 0, kHost);
  if (withOther) {
    Arrive(p, 1, kOther);
  }
  auto& actor = Standing(p, kPredator);
  actor.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &host);
  hostOut = &host;
  return actor;
}

TEST_CASE("An accepted reanimate stands a dead NPC at its host's live sample "
          "without a dead sample first; a live sample with neither stands "
          "nothing",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 1.f);
  REQUIRE(actor.IsDead());
  actor.NoteReanimateAccepted(std::chrono::steady_clock::now());
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

TEST_CASE("A new host's first live sample of a dead NPC stands nothing until "
          "that host has sampled the body dead",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 0.f);
  REQUIRE(actor.IsDead());
  {
    RawMessageData raw;
    raw.userId = 0;
    CellLoadedMessage unloaded;
    unloaded.cell = kForestCell;
    unloaded.loaded = false;
    p.GetActionListener().OnCellLoaded(raw, unloaded);
  }
  host->SetPos(kForest + NiPoint3{ 60000.f, 60000.f, 0.f });
  Host(p, 1, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &other);
  State(p, 1, kPredator, 1.f);
  REQUIRE(actor.IsDead());
  State(p, 1, kPredator, 0.f);
  REQUIRE(actor.IsDead());
  State(p, 1, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

TEST_CASE("The host's zero sample on a still-alive record is its dead-sample "
          "stamp, so a later stand-up with no claimed hit stands the body",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  State(p, 0, kPredator, 0.f);
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

TEST_CASE("A host that quit cleanly and hosts again under the same id must "
          "sample the body dead again before a live sample stands it",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, false);
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 0.f);
  REQUIRE(actor.IsDead());
  // the clean quit; the grace passes while the player is away (the Release
  // reaches no game of its); the same id back, a new login generation
  Leave(p, 0, kHost);
  p.TickCellHosts(NowMs() + skymp::rules::host_grace_ms() + 2'000);
  auto& again = Arrive(p, 0, kHost);
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &again);
  State(p, 0, kPredator, 1.f);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 0.f);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

TEST_CASE("A host that walked off, quit and came back first must sample the "
          "body dead again (its stamp named the old login)",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, false);
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 0.f);
  REQUIRE(actor.IsDead());
  // the host walks off: the cell unhosted after the grace, no Release flip
  // reaching the body; then the quit, the grace again, the same id back
  host->SetPos(kForest + NiPoint3{ 60000.f, 60000.f, 0.f });
  p.TickCellHosts(NowMs() + skymp::rules::host_grace_ms() + 2'000);
  Leave(p, 0, kHost);
  p.TickCellHosts(NowMs() + skymp::rules::host_grace_ms() + 2'000);
  auto& again = Arrive(p, 0, kHost);
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &again);
  State(p, 0, kPredator, 1.f);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 0.f);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

TEST_CASE("A bleedout's zero long before a later death is not that death's "
          "stamp; the host must sample the body dead again",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  const auto now = std::chrono::steady_clock::now();
  // the host's zero on the living actor twenty seconds ago (a test-only
  // backdate of the stamp, as RecordDeathAt is of the death)
  actor.NoteHostSampledDead(kHost, host->LoginGeneration(),
                            now - std::chrono::seconds(20));
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 1.f);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 0.f);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

TEST_CASE("A dead-sample stamp counts within kPendingDeathMs before the "
          "recorded death and not beyond it",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  const auto now = std::chrono::steady_clock::now();
  // the death four seconds ago; a stamp two seconds before it, inside the
  // window: the live sample stands the body
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  actor.NoteHostSampledDead(kHost, host->LoginGeneration(),
                            now - std::chrono::seconds(6));
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
  // the same body dead again; a stamp four seconds before the death, past
  // the window: the live sample stands nothing until a fresh dead sample
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  actor.NoteHostSampledDead(kHost, host->LoginGeneration(),
                            now - std::chrono::seconds(8));
  State(p, 0, kPredator, 1.f);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 0.f);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

TEST_CASE("A live sample inside kPendingDeathMs of the death is a sample in "
          "flight, not a resurrection, with the host's stamp or without it",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  // the death now, the host's dead sample now, a live sample at once: the
  // grace refuses it although the stamp is the witness's
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 0.f);
  REQUIRE(actor.IsDead());
  State(p, 0, kPredator, 1.f);
  REQUIRE(actor.IsDead());
  // past the grace (the stamp taken after the death counts): the body stands
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

TEST_CASE("A dead-sample stamp inside the window of the PENDING death's "
          "start counts although TickDeaths recorded the death seconds later",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  const auto now = std::chrono::steady_clock::now();
  // the pending death began 8 s ago (the server's number reached zero), the
  // host's engine died at the DamageCause and its flip landed 0.4 s later,
  // and TickDeaths recorded the death 4 s after the start (4 s ago now,
  // past the grace)
  actor.BeginPendingDeath(nullptr);
  actor.RecordPendingDeathAt(NowMs() - 8000);
  actor.NoteHostSampledDead(kHost, host->LoginGeneration(),
                            now - std::chrono::milliseconds(7600));
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  // the stamp is 3.6 s before the recorded death (refused by 600 ms under a
  // window anchored there) but 0.4 s after the pending start, so it counts
  // by 3.4 s: the body stands at the host's live sample
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

TEST_CASE("A stale pending start from an earlier life never anchors the next "
          "death's window: the stand-up clears it",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  const auto now = std::chrono::steady_clock::now();
  // life one: a pending death that began 60 s ago, the kill, the stand-up
  actor.BeginPendingDeath(nullptr);
  actor.RecordPendingDeathAt(NowMs() - 60000);
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  actor.Respawn(false);
  REQUIRE_FALSE(actor.IsDead());
  // life two: the host's zero sample 20 s ago, then a death with no pending
  // phase (OnActorDeath's kill, a script's, the console's) recorded 4 s ago:
  // the stamp is 16 s before the death, past its window, although it follows
  // life one's pending start; the live sample stands nothing
  actor.NoteHostSampledDead(kHost, host->LoginGeneration(),
                            now - std::chrono::seconds(20));
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 1.f);
  REQUIRE(actor.IsDead());
}

TEST_CASE("A death through SetIsDead during a pending death leaves the "
          "pending phase armed until the stand-up clears it, so the next "
          "pending death arms",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.BeginPendingDeath(nullptr);
  REQUIRE(actor.HasPendingDeath());
  // the death lands by the property, not by Kill (the lab's isDead write)
  actor.SetIsDead(true);
  REQUIRE(actor.IsDead());
  actor.Respawn(false);
  REQUIRE_FALSE(actor.IsDead());
  REQUIRE_FALSE(actor.HasPendingDeath());
  REQUIRE(p.worldState.pendingDeaths.count(kPredator) == 0);
  actor.BeginPendingDeath(nullptr);
  REQUIRE(actor.HasPendingDeath());
}

TEST_CASE("The host's reliable death report is its dead-sample stamp",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  // the host reports the death (a fall, with the height samples the fall
  // rule checks): the record dies and the report is the stamp; no
  // hosted-state zero follows
  const NiPoint3 body = actor.GetPos();
  const auto fell = std::chrono::steady_clock::now();
  actor.RecordHeight(body.z + 500.f, fell - std::chrono::seconds(2));
  actor.RecordHeight(body.z, fell);
  Report(p, 0, kPredator, 1, 0, body);
  REQUIRE(actor.IsDead());
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 1.f);
  REQUIRE_FALSE(actor.IsDead());
}

// thuum docs/verbs/npc-fate.md (playtest 37's finding 54): a body dead since
// before this server process has no death stamp (RecordDeathAt runs at
// SendAndSetDeathState alone, nothing stamps a body loaded dead). Playtest
// 40's F9 (2026-11-02): the host's engine loads such a corpse alive for a
// frame before the kill word lands, and that first live sample stood the
// body up after a restart and a relaunch; so a live sample stands a body
// dead before this process only after its host has sampled it dead once
TEST_CASE("A hosted NPC dead since before this server process stands again "
          "at its host's live sample only after its host has sampled it "
          "dead once",
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
  // the kill stamped the death on this process's clock; the default time
  // point is what a body loaded dead carries (MpActor::DiedAgo answers
  // nothing for it)
  actor.RecordDeathAt(std::chrono::steady_clock::time_point());
  REQUIRE_FALSE(actor.DiedAgo(std::chrono::steady_clock::now()).has_value());

  // from the other game: dropped at the host gate as any sample of its
  State(p, 1, kPredator, 1.f);
  REQUIRE(actor.IsDead());

  // from the host, before any dead sample: the corpse's loading frame, not
  // a resurrection (playtest 40's F9)
  State(p, 0, kPredator, 1.f);
  REQUIRE(actor.IsDead());
  // the host samples it dead, then alive: the engine's own respawn, taken,
  // the death cleared, the live word to the other game
  State(p, 0, kPredator, 0.f);
  REQUIRE(actor.IsDead());
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

// (a script's kill, cause 3, was refused here until ADR-062 decision 2 made
// it the host's word; its case is the last in this file)
TEST_CASE("An unnamed cause and a body far from the record are refused, a "
          "dead actor's report is nothing, and the setting off refuses "
          "everything",
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

// thuum docs/verbs/npc-fate.md (playtest 40's F5, 2026-11-02): a reanimated
// body's spell ends and the engine kills it with no hit, drop or activation
// the server saw; the host's report of that death is the reanimation's end
// when the server stood the body up after an accepted reanimate within
// kReanimatedEndMs, and nothing without the stand-up
TEST_CASE("A host's unnamed-cause report on a body the server stood up after "
          "a reanimate is the reanimation's end; without the stand-up it is "
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
  REQUIRE(p.SimulatorOf(actor) == kHost);
  const NiPoint3 body = actor.GetPos();
  const auto now = std::chrono::steady_clock::now();
  // no stand-up on record: the unnamed cause has nothing behind it
  Report(p, 0, kPredator, 0, 0, body);
  REQUIRE_FALSE(actor.IsDead());
  // a reanimate accepted and the host's engine stood the body up 40 s ago
  actor.NoteReanimateAccepted(now - std::chrono::seconds(45));
  actor.NoteHostStoodUp(now - std::chrono::seconds(40));
  Report(p, 0, kPredator, 0, 0, body);
  REQUIRE(actor.IsDead());
  StandUp(p, actor);
  // too long after the stand-up: the spell's span is over, nothing behind it
  actor.NoteHostStoodUp(now - std::chrono::seconds(120));
  Report(p, 0, kPredator, 0, 0, body);
  REQUIRE_FALSE(actor.IsDead());
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

// thuum docs/verbs/npc-fate.md ("The body's rest", ADR-067 decision 4;
// physics rung 5a, ADR-071 decision 5): a corpse is a physics object, a row
// of the prop table from its death to its stand-up. The engine that
// simulated the NPC at its death owns its row: for WorldState::bodySettleMs
// its movement reports for the dead actor move the record and reach the
// other games (R2, recorded not judged); any other seat's are refused
// (E_PROP_NOT_OWNER); with no simulator at the death, or once the row
// returned (the owner's departure or unload, the dwell's rest), the owner is
// the cell's host. Accepted samples held within MpActor::kBodyRestUnits and
// kBodyRestDegrees of the dwell's anchor for kBodyRestDwellMs, no sooner
// than kBodyRestMinMs after the death (which returns the row), or the
// window's end (PartOne::TickBodies, which leaves the row), put the body at
// rest, and every later report is refused (E_BODY_AT_REST) until a
// resurrection clears it and ends the row. A dead actor's movement report is
// a body sample and never a stand-up: the stand-up reads the health the host
// samples (OnHostedActorState), not the body's place.

namespace {
// `user`'s movement report for `actorId` at `pos` and `rot` (CellHostTest's
// Move idiom). Its health and isDead are a live actor's on purpose: the body
// path reads neither
void MoveBody(PartOne& p, Networking::UserId user, uint32_t actorId,
              const NiPoint3& pos, const NiPoint3& rot = { 0.f, 0.f, 0.f })
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = p.worldState.GetFormAt<MpActor>(actorId).GetIdx();
  msg.data.worldOrCell = kTamriel;
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { rot.x, rot.y, rot.z };
  msg.data.runMode = "Standing";
  msg.data.healthPercentage = 1.f;
  msg.data.isDead = false;
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// `user`'s game reports `cell` loaded or unloaded
void CellReport(PartOne& p, Networking::UserId user, uint32_t cell,
                bool loaded)
{
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage msg;
  msg.cell = cell;
  msg.loaded = loaded;
  p.GetActionListener().OnCellLoaded(raw, msg);
}

// the owner words (PropOwner) `user` heard for `refrId`
std::vector<PropOwnerMessage> OwnerWordsTo(PartOne& p, Networking::UserId user,
                                           uint32_t refrId)
{
  std::vector<PropOwnerMessage> out;
  for (auto& m : p.Messages()) {
    auto word = dynamic_cast<PropOwnerMessage*>(m.message.get());
    if (m.userId == user && word && word->refr == refrId) {
      out.push_back(*word);
    }
  }
  return out;
}

// the movement relays `user` received since the last clear: the raw `{}`
// bytes the fixture parses to an empty object; `reliable` counts those sent
// reliably alone
size_t RelaysTo(PartOne& p, Networking::UserId user, bool reliable = false)
{
  size_t n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() && m.j.empty() &&
        (!reliable || m.reliable)) {
      ++n;
    }
  }
  return n;
}

// the isDead "false" words `user` heard for `refrId` (a stand-up's)
size_t LiveWordsTo(PartOne& p, Networking::UserId user, uint32_t refrId)
{
  size_t n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::UpdateProperty) &&
        m.j.value("propName", std::string()) == "isDead" &&
        m.j.value("refrId", 0u) == refrId &&
        m.j.value("dataDump", std::string()) == "false") {
      ++n;
    }
  }
  return n;
}
}

TEST_CASE("The body's rest: its owner's movement report for a dead NPC inside "
          "the settle window moves the record and reaches the other game",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  const NiPoint3 deathPoint = actor.GetPos();
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  REQUIRE(actor.HasBodySettle());
  REQUIRE(actor.IsBodySettling());
  REQUIRE_FALSE(actor.IsBodyAtRest());
  REQUIRE(actor.BodyRestAtMs() == 0);
  REQUIRE(p.PropLiveOwner(actor) == kHost);
  REQUIRE(p.worldState.settlingBodies.count(kPredator) == 1);

  // the ragdoll slid down the slope (playtest 42's F4): the record follows
  const NiPoint3 slid = deathPoint + NiPoint3{ 700.f, 0.f, -300.f };
  const NiPoint3 lying{ 90.f, 0.f, 45.f };
  p.Messages().clear();
  MoveBody(p, 0, kPredator, slid, lying);
  REQUIRE(actor.GetPos() == slid);
  REQUIRE(actor.GetAngle() == lying);
  REQUIRE(actor.IsDead());
  REQUIRE(actor.IsBodySettling());
  REQUIRE(actor.BodySampleCount() == 1);
  // relayed to the other game as a live hosted actor's step, unreliable
  REQUIRE(RelaysTo(p, 1) == 1);
  REQUIRE(RelaysTo(p, 1, true) == 0);

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's rest: a report from a game that does not own the body "
          "is refused (E_PROP_NOT_OWNER) and moves nothing",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.Kill(nullptr);
  REQUIRE(p.PropLiveOwner(actor) == kHost);
  const NiPoint3 deathPoint = actor.GetPos();

  p.Messages().clear();
  MoveBody(p, 1, kPredator, deathPoint + NiPoint3{ 300.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint);
  REQUIRE(actor.BodySampleCount() == 0);
  REQUIRE(actor.IsBodySettling());
  REQUIRE(RelaysTo(p, 0) == 0);
  REQUIRE(RelaysTo(p, 1) == 0);

  // the owner's report right after is taken: the refusal held nothing back
  MoveBody(p, 0, kPredator, deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.BodySampleCount() == 1);

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's rest: samples held within 2 units and 2 degrees of the "
          "dwell's anchor for the dwell, no sooner than its floor after the "
          "death, put the body at rest; two agreeing samples at the death "
          "point, a creep and a turn do not",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.Kill(nullptr);
  const NiPoint3 deathPoint = actor.GetPos();
  // the samples against the record's own death stamp: no wall-clock slack
  const uint64_t d = actor.BodyDiedAtMs();
  REQUIRE(d != 0);
  REQUIRE(MpActor::kBodyRestDwellMs == 750);
  REQUIRE(MpActor::kBodyRestMinMs == 2'500);
  const NiPoint3 lying{ 0.f, 0.f, 10.f };

  // b-npc-death's red (rung 3's gate 20261107-005511): the engine's reference
  // holds the death point while the ragdoll starts, so its first samples
  // agree 133 ms apart; however long they agree, no rest before the floor
  REQUIRE_FALSE(actor.NoteBodySample(deathPoint, lying, d + 50, kHost));
  REQUIRE_FALSE(actor.NoteBodySample(deathPoint, lying, d + 183, kHost));
  REQUIRE_FALSE(actor.NoteBodySample(deathPoint, lying, d + 1'500, kHost));
  REQUIRE_FALSE(actor.IsBodyAtRest());
  REQUIRE(actor.BodySampleCount() == 3);

  // then the ragdoll slides: a sample past the anchor's bounds starts the
  // dwell again there
  const NiPoint3 slid = deathPoint + NiPoint3{ 400.f, 0.f, -150.f };
  REQUIRE_FALSE(actor.NoteBodySample(slid, lying, d + 2'730, kHost));
  // a creep: each step 1.5 units from the last, which two agreeing samples
  // called a rest before; the second is 3 units from the anchor, so the
  // dwell starts again at it
  const NiPoint3 crept = slid + NiPoint3{ 3.f, 0.f, 0.f };
  REQUIRE_FALSE(actor.NoteBodySample(slid + NiPoint3{ 1.5f, 0.f, 0.f }, lying,
                                     d + 3'000, kHost));
  REQUIRE_FALSE(actor.NoteBodySample(crept, lying, d + 3'130, kHost));
  // a turn of 3 degrees in place: the angle starts it again
  REQUIRE_FALSE(
    actor.NoteBodySample(crept, NiPoint3{ 0.f, 0.f, 13.f }, d + 3'260, kHost));
  // 359 is 14 degrees from 13 through the heading's wrap: again
  REQUIRE_FALSE(actor.NoteBodySample(crept, NiPoint3{ 0.f, 0.f, 359.f },
                                     d + 3'390, kHost));
  REQUIRE_FALSE(actor.IsBodyAtRest());
  // 1 unit off the anchor and 1.5 degrees through the wrap (359 to 0.5):
  // 510 ms into the dwell is short of it, 750 ms ends it
  REQUIRE_FALSE(actor.NoteBodySample(crept + NiPoint3{ 1.f, 0.f, 0.f },
                                     NiPoint3{ 0.f, 0.f, 0.5f }, d + 3'900,
                                     kHost));
  REQUIRE(actor.NoteBodySample(crept + NiPoint3{ 1.f, 0.f, 0.f },
                               NiPoint3{ 0.f, 0.f, 0.5f }, d + 4'140, kHost));
  REQUIRE(actor.IsBodyAtRest());
  REQUIRE_FALSE(actor.IsBodySettling());
  REQUIRE(actor.BodyRestAtMs() == d + 4'140);
  REQUIRE(actor.BodySampleCount() == 10);
  REQUIRE(p.worldState.settlingBodies.count(kPredator) == 0);

  // a body still from its first sample rests at the floor, not before
  StandUp(p, actor);
  actor.Kill(nullptr);
  const uint64_t d2 = actor.BodyDiedAtMs();
  REQUIRE_FALSE(actor.NoteBodySample(deathPoint, lying, d2 + 100, kHost));
  REQUIRE_FALSE(actor.NoteBodySample(deathPoint, lying, d2 + 2'400, kHost));
  REQUIRE(actor.NoteBodySample(deathPoint, lying, d2 + 2'530, kHost));
  REQUIRE(actor.BodyRestAtMs() == d2 + 2'530);

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's rest: the owner's sample that ends the dwell puts the "
          "body at rest through its report, goes reliably, and the next "
          "report is refused (E_BODY_AT_REST)",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.Kill(nullptr);
  const NiPoint3 deathPoint = actor.GetPos();

  // the death and the dwell's anchor behind the report's own clock (the
  // movement path reads steady_clock, as NowMs does): the death 5 s ago, the
  // body still at `rest` since 1 s ago
  const uint64_t now = NowMs();
  actor.BeginBodySettle(now - 5'000);
  const NiPoint3 rest = deathPoint + NiPoint3{ 211.f, 0.f, -50.f };
  REQUIRE_FALSE(actor.NoteBodySample(rest, NiPoint3{ 0.f, 0.f, 359.f },
                                     now - 1'000, kHost));
  REQUIRE_FALSE(actor.IsBodyAtRest());
  // the owner's next report, 1 unit off and 1.5 degrees through the wrap:
  // the dwell is over, the body at rest at this sample
  p.Messages().clear();
  const NiPoint3 last = rest + NiPoint3{ 1.f, 0.f, 0.f };
  MoveBody(p, 0, kPredator, last, NiPoint3{ 0.f, 0.f, 0.5f });
  REQUIRE(actor.IsBodyAtRest());
  REQUIRE_FALSE(actor.IsBodySettling());
  REQUIRE(actor.BodyRestAtMs() != 0);
  REQUIRE(actor.BodySampleCount() == 2);
  REQUIRE(actor.GetPos() == last);
  REQUIRE(p.worldState.settlingBodies.count(kPredator) == 0);
  REQUIRE(RelaysTo(p, 1, true) == 1);

  // from then every report for the body is refused, the owner's included
  const auto restAt = actor.BodyRestAtMs();
  p.Messages().clear();
  MoveBody(p, 0, kPredator, last + NiPoint3{ 500.f, 0.f, 0.f });
  MoveBody(p, 1, kPredator, last + NiPoint3{ 500.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == last);
  REQUIRE(actor.BodySampleCount() == 2);
  REQUIRE(actor.BodyRestAtMs() == restAt);
  REQUIRE(RelaysTo(p, 1) == 0);
  REQUIRE(RelaysTo(p, 0) == 0);

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's rest: the settle window's end (bodySettleMs) puts the "
          "body at rest where the record has it, by the sweep or by the "
          "next report",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.bodySettleMs = 5'000;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.Kill(nullptr);
  const NiPoint3 deathPoint = actor.GetPos();
  // the ticks against the record's own death stamp: no wall-clock slack
  const uint64_t d = actor.BodyDiedAtMs();
  REQUIRE(d != 0);

  // the sweep inside the window: nothing; past it: at rest at the record
  p.TickBodies(d + 2'500);
  REQUIRE_FALSE(actor.IsBodyAtRest());
  REQUIRE(actor.IsBodySettling());
  p.TickBodies(d + 7'000);
  REQUIRE(actor.IsBodyAtRest());
  REQUIRE(actor.BodyRestAtMs() == d + 7'000);
  REQUIRE(actor.GetPos() == deathPoint);
  REQUIRE(p.worldState.settlingBodies.count(kPredator) == 0);
  MoveBody(p, 0, kPredator, deathPoint + NiPoint3{ 300.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint);

  // the same end read by the movement path when no sweep ran since: a
  // report after the window is refused, the body at rest
  StandUp(p, actor);
  p.worldState.bodySettleMs = 0;
  actor.Kill(nullptr);
  REQUIRE(actor.IsBodySettling());
  MoveBody(p, 0, kPredator, deathPoint + NiPoint3{ 300.f, 0.f, 0.f });
  REQUIRE(actor.IsBodyAtRest());
  REQUIRE(actor.GetPos() == deathPoint);
  REQUIRE(actor.BodySampleCount() == 0);
  p.worldState.bodySettleMs = 15'000;

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's rest: a respawn clears the rest, and the next death "
          "settles anew under its simulator",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.Kill(nullptr);
  const uint32_t died = p.PropEpochOf(actor);
  const NiPoint3 deathPoint = actor.GetPos();
  const NiPoint3 rest = deathPoint + NiPoint3{ 150.f, 0.f, 0.f };
  // at rest by the dwell, against the record's own death stamp
  const uint64_t d = actor.BodyDiedAtMs();
  actor.NoteBodySample(rest, NiPoint3{ 0.f, 0.f, 0.f }, d + 2'000, kHost);
  actor.NoteBodySample(rest, NiPoint3{ 0.f, 0.f, 0.f }, d + 3'000, kHost);
  REQUIRE(actor.IsBodyAtRest());

  actor.Respawn(false);
  REQUIRE_FALSE(actor.IsDead());
  REQUIRE_FALSE(actor.IsBodyAtRest());
  REQUIRE_FALSE(actor.IsBodySettling());
  REQUIRE_FALSE(actor.HasBodySettle());
  REQUIRE(actor.BodyRestAtMs() == 0);
  // the row gone at a new epoch, above the rest's
  REQUIRE(p.PropLiveOwner(actor) == 0);
  REQUIRE(p.PropEpochOf(actor) == died + 2);
  REQUIRE(actor.BodySampleCount() == 0);
  REQUIRE(p.worldState.settlingBodies.count(kPredator) == 0);

  actor.Kill(nullptr);
  REQUIRE(actor.IsBodySettling());
  REQUIRE_FALSE(actor.IsBodyAtRest());
  REQUIRE(p.PropLiveOwner(actor) == kHost);
  REQUIRE(p.PropEpochOf(actor) == died + 3);
  REQUIRE(actor.BodySampleCount() == 0);
  REQUIRE(p.worldState.settlingBodies.count(kPredator) == 1);

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's rest: a dead actor's movement report is a body sample "
          "and never a stand-up, whatever health the message carries",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  // the fate's day and its due as the kill left them (scheduled by the
  // respawn path when it ran): the body sample must change neither
  const bool dayStamped = actor.GetChangeForm().fateDiedAt.has_value();
  const size_t due = p.worldState.fateRespawnDue.count(kPredator);
  // past the grace and with the host's dead stamp: the state a live HOSTED
  // STATE sample would stand the body from (the first case of this file)
  BackdatePastGrace(actor);
  State(p, 0, kPredator, 0.f);
  const NiPoint3 deathPoint = actor.GetPos();

  p.Messages().clear();
  // MoveBody's message says health 1.0 and isDead false: not read
  MoveBody(p, 0, kPredator, deathPoint + NiPoint3{ 120.f, 0.f, -40.f });
  REQUIRE(actor.IsDead());
  REQUIRE(actor.GetChangeForm().fateDiedAt.has_value() == dayStamped);
  REQUIRE(p.worldState.fateRespawnDue.count(kPredator) == due);
  REQUIRE(LiveWordsTo(p, 1, kPredator) == 0);
  REQUIRE(actor.GetPos() == deathPoint + NiPoint3{ 120.f, 0.f, -40.f });
  REQUIRE(actor.IsBodySettling());

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's rest: the mover's unload of the cell mid-ragdoll "
          "returns the body's row to the host with its word: the new host's "
          "in-cell samples are taken, the mover's are refused "
          "(E_PROP_NOT_OWNER), and a roll into the next cell keeps the "
          "cell-change rule",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  actor.Kill(nullptr);
  REQUIRE(p.PropLiveOwner(actor) == kHost);
  const uint32_t died = p.PropEpochOf(actor);
  const NiPoint3 deathPoint = actor.GetPos();

  // the mover's game unloads the cell: its 3D of the body went with the
  // cell, so the row returns to the host by default with the word (physics
  // rung 5a, D-PH2 A), the settle running on
  p.Messages().clear();
  CellReport(p, 0, kForestCell, false);
  REQUIRE(actor.IsBodySettling());
  REQUIRE(p.PropLiveOwner(actor) == 0);
  REQUIRE(p.PropEpochOf(actor) == died + 1);
  const auto words = OwnerWordsTo(p, 1, kPredator);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].player == 0);
  REQUIRE(words[0].epoch == died + 1);

  // the mover walks away; the other game hosts
  host->SetPos(kForest + NiPoint3{ 60000.f, 60000.f, 0.f });
  Host(p, 1, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &other);
  REQUIRE(p.PropOwnerOf(actor) == kOther);

  // the new host's in-cell sample: the owner's now
  p.Messages().clear();
  MoveBody(p, 1, kPredator, deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.BodySampleCount() == 1);

  // the mover's sample: no longer the owner
  MoveBody(p, 0, kPredator, deathPoint + NiPoint3{ 200.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.BodySampleCount() == 1);

  // the new owner's roll into the next cell, which its game never reported
  // loaded: the cell-change rule refuses it
  MoveBody(p, 1, kPredator, deathPoint + NiPoint3{ 4000.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.BodySampleCount() == 1);

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's row: a death under a dialogue's claim is the "
          "claimant's: its samples move the record, the cell's host's are "
          "refused (E_PROP_NOT_OWNER)",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  // the other game talks to the predator: its claim names it the simulator
  CellReport(p, 1, kForestCell, true);
  REQUIRE(p.ClaimActor(other, actor) == PartOne::ClaimVerdict::Granted);
  REQUIRE(p.SimulatorOf(actor) == kOther);
  const uint32_t before = p.PropEpochOf(actor);

  actor.Kill(nullptr);
  REQUIRE(p.PropLiveOwner(actor) == kOther);
  REQUIRE(p.PropOwnerOf(actor) == kOther);
  REQUIRE(p.PropEpochOf(actor) == before + 1);
  const NiPoint3 deathPoint = actor.GetPos();

  // the cell's host is no owner of the claimant's body
  p.Messages().clear();
  MoveBody(p, 0, kPredator, deathPoint + NiPoint3{ 200.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint);
  REQUIRE(actor.BodySampleCount() == 0);
  REQUIRE(RelaysTo(p, 1) == 0);

  // the claimant's sample is taken and reaches the host's game
  MoveBody(p, 1, kPredator, deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.BodySampleCount() == 1);
  REQUIRE(RelaysTo(p, 0) == 1);

  p.ReleaseClaim(other, actor);
  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's row: a death nobody simulates is the cell's host's "
          "alone: the first host's samples move the record and a claimant "
          "after the death is refused (E_PROP_NOT_OWNER), the declared "
          "narrowing",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  auto& host = Arrive(p, 0, kHost);
  auto& other = Arrive(p, 1, kOther);
  auto& actor = Standing(p, kPredator);
  actor.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  // any earlier host past its grace: nobody hosts the forest's cell
  p.TickCellHosts(NowMs() + skymp::rules::host_grace_ms() + 2'000);
  REQUIRE(p.worldState.HostOf(actor) == nullptr);

  actor.Kill(nullptr);
  REQUIRE(actor.IsBodySettling());
  REQUIRE(p.PropLiveOwner(actor) == 0);
  REQUIRE(p.PropOwnerOf(actor) == 0);
  const NiPoint3 deathPoint = actor.GetPos();

  // the first host's by default, no row naming it
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &host);
  REQUIRE(p.PropLiveOwner(actor) == 0);
  REQUIRE(p.PropOwnerOf(actor) == kHost);

  // a claim after the death names the other game the actor's simulator by
  // the cells rule; the body is not its (canon's first simulator to report
  // claimed it)
  CellReport(p, 1, kForestCell, true);
  REQUIRE(p.ClaimActor(other, actor) == PartOne::ClaimVerdict::Granted);
  REQUIRE(p.SimulatorOf(actor) == kOther);
  p.Messages().clear();
  MoveBody(p, 1, kPredator, deathPoint + NiPoint3{ 200.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint);
  REQUIRE(actor.BodySampleCount() == 0);
  REQUIRE(RelaysTo(p, 0) == 0);

  // the host's in-cell sample is taken past the cell-host rule
  MoveBody(p, 0, kPredator, deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.BodySampleCount() == 1);
  REQUIRE(RelaysTo(p, 1) == 1);

  p.ReleaseClaim(other, actor);
  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's row: the simulator leaving mid-settle returns the row "
          "with the word to the other game, and the next host's samples "
          "move the record",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.Kill(nullptr);
  REQUIRE(p.PropLiveOwner(actor) == kHost);
  const uint32_t died = p.PropEpochOf(actor);
  const NiPoint3 deathPoint = actor.GetPos();

  // the departure returns every live row naming the leaver, a body's too,
  // each with the word (player 0) to the games still listening
  p.Messages().clear();
  Leave(p, 0, kHost);
  REQUIRE(actor.IsBodySettling());
  REQUIRE(p.PropLiveOwner(actor) == 0);
  REQUIRE(p.PropEpochOf(actor) == died + 1);
  const auto words = OwnerWordsTo(p, 1, kPredator);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].player == 0);
  REQUIRE(words[0].epoch == died + 1);

  // past the leaver's grace the other game hosts, and owns the body
  Host(p, 1, kForestCell);
  REQUIRE(p.PropOwnerOf(actor) == kOther);
  MoveBody(p, 1, kPredator, deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.BodySampleCount() == 1);

  StandUp(p, actor);
  Leave(p, 1, kOther);
}

TEST_CASE("The body's row: a death inside a departed host's grace names "
          "nobody (the leaver still hosts the cell, but no user plays it), "
          "so the next host's samples move the record",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  const uint32_t before = p.PropEpochOf(actor);

  // the host's game quits, its actor left in the world (production
  // disables it at the disconnect): through its grace the cells rule still
  // names it the cell's host, and its departure's return has run already
  DoDisconnect(p, 0);
  REQUIRE(p.serverState.UserByActor(host) == Networking::InvalidUserId);
  REQUIRE(p.worldState.HostOf(actor) == host);

  // a death inside the grace: the row names nobody, the cell's host by
  // default, never the leaver, whose departure's return has run already
  actor.Kill(nullptr);
  REQUIRE(actor.IsBodySettling());
  REQUIRE(p.PropLiveOwner(actor) == 0);
  REQUIRE(p.PropEpochOf(actor) == before + 1);
  const NiPoint3 deathPoint = actor.GetPos();

  // past the grace the other game hosts, and its samples move the record
  Host(p, 1, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &other);
  REQUIRE(p.PropOwnerOf(actor) == kOther);
  MoveBody(p, 1, kPredator, deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.GetPos() == deathPoint + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(actor.BodySampleCount() == 1);

  // the shared world: the leaver's actor goes too, past its grace, so the
  // next case's arrival creates it afresh
  StandUp(p, actor);
  Leave(p, 1, kOther);
  p.DestroyActor(kHost);
}

TEST_CASE("The body's row: a death after the simulator's game unloaded the "
          "cell names nobody (its 3D of the body went with the cell), the "
          "cell's host's by default",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  constexpr uint32_t kForestCell = 0x9810;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, false);
  const uint32_t before = p.PropEpochOf(actor);

  // the host's game unloads the cell: it still hosts through its grace, so
  // the cells rule names it the actor's simulator at the death
  CellReport(p, 0, kForestCell, false);
  REQUIRE(p.worldState.HostOf(actor) == host);
  REQUIRE(p.SimulatorOf(actor) == kHost);

  // the death: the row names nobody (wire-rules props died's unload guard,
  // D-PH2 A), where naming the unloaded seat would strand a live row
  // nobody streams until its departure
  actor.Kill(nullptr);
  REQUIRE(actor.IsBodySettling());
  REQUIRE(p.PropLiveOwner(actor) == 0);
  REQUIRE(p.PropEpochOf(actor) == before + 1);

  StandUp(p, actor);
  Leave(p, 0, kHost);
}

TEST_CASE("The body's row: the dwell's rest returns it to the host at a new "
          "epoch; the settle window's end leaves it as it stood",
          "[ActorDeath][BodyRest][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpActor* host = nullptr;
  auto& actor = HostedPredator(p, host, true);
  actor.Kill(nullptr);
  REQUIRE(p.PropLiveOwner(actor) == kHost);
  const uint32_t died = p.PropEpochOf(actor);
  const NiPoint3 deathPoint = actor.GetPos();

  // still for the dwell, against the record's own death stamp: the core's
  // rest is the body's asleep sample
  const uint64_t d = actor.BodyDiedAtMs();
  REQUIRE_FALSE(actor.NoteBodySample(deathPoint, NiPoint3{ 0.f, 0.f, 0.f },
                                     d + 2'000, kHost));
  REQUIRE(actor.NoteBodySample(deathPoint, NiPoint3{ 0.f, 0.f, 0.f },
                               d + 3'000, kHost));
  REQUIRE(actor.IsBodyAtRest());
  REQUIRE(p.PropLiveOwner(actor) == 0);
  REQUIRE(p.PropEpochOf(actor) == died + 1);
  REQUIRE(p.PropOwnerOf(actor) == kHost);

  // the window's end is a clock: the body at rest, its row the simulator's
  // still at the death's epoch
  StandUp(p, actor);
  REQUIRE(p.PropEpochOf(actor) == died + 2);
  p.worldState.bodySettleMs = 5'000;
  actor.Kill(nullptr);
  REQUIRE(p.PropLiveOwner(actor) == kHost);
  REQUIRE(p.PropEpochOf(actor) == died + 3);
  const uint64_t d2 = actor.BodyDiedAtMs();
  p.TickBodies(d2 + 7'000);
  REQUIRE(actor.IsBodyAtRest());
  REQUIRE(p.PropLiveOwner(actor) == kHost);
  REQUIRE(p.PropEpochOf(actor) == died + 3);
  p.worldState.bodySettleMs = 15'000;

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}
// thuum docs/DECISIONS.md ADR-062 decision 2 (Eli, playtest 41: "trust
// should LARGELY be the default"): a host-run script's kill, reported by the
// host whose engine ran it, is the host's word (R2, declared unvalidated);
// before ADR-062 cause 3 was refused as the server's to run
TEST_CASE("A host's report of a script's kill (cause 3) is taken as the "
          "host's word; another game's is not the host's to make",
          "[ActorDeath][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  constexpr uint32_t kForestCell = 0x9810;
  auto& host = Arrive(p, 0, kHost);
  Arrive(p, 1, kOther);
  auto& actor = Standing(p, kPredator);
  actor.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &host);

  // the other game does not simulate the actor: its report is nobody's
  Report(p, 1, kPredator, 3, 0, actor.GetPos());
  REQUIRE_FALSE(actor.IsDead());

  // the host's: taken, the killer the script named credited; the body's
  // distance to the record is no gate for a script's kill (ADR-063 decision
  // 4: the host streams the record itself; KillEssential's flag the same)
  Report(p, 0, kPredator, 3, kHost,
         actor.GetPos() + NiPoint3{ 3000.f, 0.f, 0.f });
  REQUIRE(actor.IsDead());

  StandUp(p, actor);
  Leave(p, 1, kOther);
  Leave(p, 0, kHost);
}

// thuum docs/DECISIONS.md ADR-062 decision 2 and ADR-063 decision 4 (slice
// 1b's review, finding 7): the engine's essential rule is a check on the
// owner's engine, not on consistency (KillEssential clears the flag and
// kills; the server's base still reads essential), so a host-run script's
// kill (cause 3) passes MayDie where an actor's hit (cause 4) is refused
TEST_CASE("A host's report of a script's kill on an essential base is taken "
          "where an actor's hit on it is refused as immortal",
          "[ActorDeath][espm]")
{
  // Delphine 0x13485 in the Sleeping Giant 0x133c6 (lab/esm.py refs-of
  // 0x13478: her base's ACBS flags carry Essential); the host stands inside
  // the inn so its loaded report is granted the interior
  constexpr uint32_t kInn = 0x133c6;
  constexpr uint32_t kDelphine = 0x13485;
  const NiPoint3 kInside{ -250.f, -360.f, 0.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  DoConnect(p, 0);
  p.CreateActor(kHost, kInside, 0, kInn);
  p.SetUserActor(0, kHost);
  p.TickCellHosts(NowMs());
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& actor = Standing(p, kDelphine);
  Host(p, 0, kInn);
  REQUIRE(p.worldState.HostOf(actor) == &host);
  REQUIRE_FALSE(actor.MayDie(nullptr));

  // an actor's hit on her, the hit the server numbered a moment before so
  // that only the essential rule can refuse it: refused
  actor.NoteHitBy(kHost, std::chrono::steady_clock::now());
  Report(p, 0, kDelphine, 4, kHost, actor.GetPos());
  REQUIRE_FALSE(actor.IsDead());

  // a script's kill on her: the host's word, taken
  Report(p, 0, kDelphine, 3, 0, actor.GetPos());
  REQUIRE(actor.IsDead());

  StandUp(p, actor);
  Leave(p, 0, kHost);
}
