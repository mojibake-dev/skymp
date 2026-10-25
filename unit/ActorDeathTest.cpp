#include "ActionListener.h"
#include "ActivateMessage.h"
#include "ActorDeathMessage.h"
#include "CellLoadedMessage.h"
#include "TestUtils.hpp"
#include "libespm/Loader.h"
#include "wire_bridge_cxx/rules.h"
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
