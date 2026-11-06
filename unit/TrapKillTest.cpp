#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum docs/verbs/traps.md (playtest 47, 2026-11-06): since ADR-068 D-S3 a
// family pair (a plate and its spike wall) is fired on every seat and never
// reported, so a host's death report for an NPC a trap killed found no
// evidence and was refused, E_DEATH_UNCHECKED (the blade wall's 9bcb9, the
// oil fire's 9b0f9), the record walking the NPC on until a player killed it
// again. A trap's evidence now is its trigger's own firing and the
// trigger's activation children, a host's linked report or a destruction
// record, kept kDeathTrapWindow (30 s, UNMEASURED) against a player
// activation's 5 s. The stage, Bleak Falls Temple 0x371de's game data (lab
// esm reads, TrapRelayTest's and DestructionStageTest's): the plate
// TrapStonePressurePlate02 0x567f2 at (8589.1, -5338.0) and its spike wall
// 0x54b15 (its XAPR names the plate) at (8428.4, -5421.5); the draugr
// 0x380b3 (an ACHR) at (8657, -5215, -2055); the oil lamp 0x9b0fb, a
// destructible ACTI, at (5779, -7656, -2131).
namespace {
constexpr uint32_t kHostActor = 0xff000fd1;
constexpr uint32_t kGuestActor = 0xff000fd2;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPlate = 0x567f2;
constexpr uint32_t kWall = 0x54b15;
constexpr uint32_t kDraugr = 0x380b3;
constexpr uint32_t kLamp = 0x9b0fb;
// the review's three: the oil pool 0x9b0ea burning in Bleak Falls Sanctum
// 0x371dd (playtest 47's fire); the Temple's trigger box 0x86018 and the
// hanging blade 0x3ac33 its linked report names (47's "86018 activates
// 3ac33 ...: relayed"); Embershard Mine's rockfall, the wire 0xb6d3f, its
// linker 0xb6d3e and the rigged platform 0xb6d47, the wire's child's child
constexpr uint32_t kSanctum = 0x371dd;
constexpr uint32_t kPool = 0x9b0ea;
constexpr uint32_t kBoxTrigger = 0x86018;
constexpr uint32_t kBlade = 0x3ac33;
constexpr uint32_t kWire = 0xb6d3f;
constexpr uint32_t kLinker = 0xb6d3e;
constexpr uint32_t kPlatform = 0xb6d47;

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("trap-kill-test", sink);
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

// the host (user 0) and a guest (user 1) beside the plate in the temple, the
// host hosting it and both games reporting it loaded; NPCs on and the
// draugr loaded, simulated by the host; the wall loaded, so the plate's
// activation children name it (WorldState's XAPR map)
MpActor& Enter(PartOne& p)
{
  p.worldState.npcEnabled = true;
  p.worldState.npcDeathReports = true;
  auto& plate = p.worldState.GetFormAt<MpObjectReference>(kPlate);
  p.worldState.GetFormAt<MpObjectReference>(kWall);
  const NiPoint3 by = plate.GetPos() + NiPoint3{ 100.f, 0.f, 0.f };
  DoConnect(p, 0);
  p.CreateActor(kHostActor, by, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, by, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(by);
    actor.ForceSubscriptionsUpdate();
  }
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  for (Networking::UserId user : { 0u, 1u }) {
    RawMessageData raw;
    raw.userId = user;
    CellLoadedMessage loaded;
    loaded.cell = kTemple;
    loaded.loaded = true;
    p.GetActionListener().OnCellLoaded(raw, loaded);
  }
  p.TickCellHosts(past);
  REQUIRE(p.GetCellHost(kTemple).hosted);
  REQUIRE(p.GetCellHost(kTemple).player == kHostActor);
  auto& draugr = p.worldState.GetFormAt<MpActor>(kDraugr);
  REQUIRE(p.SimulatorOf(draugr) == kHostActor);
  REQUIRE_FALSE(draugr.IsDead());
  return draugr;
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

// the host's death report of `refrId` at `pos`, the cause unnamed and no
// killer, as DeathReportService sends a trap's kill
void ReportDeath(PartOne& p, uint32_t refrId, const NiPoint3& pos)
{
  ActorDeathMessage msg;
  msg.refrId = refrId;
  msg.cause = 0;
  msg.killer = 0;
  msg.pos = { pos.x, pos.y, pos.z };
  RawMessageData raw;
  raw.userId = 0;
  p.GetActionListener().OnActorDeath(raw, msg);
}

// the host's report of a trigger's own firing (activationService: caster
// and target the same)
void TriggerFired(PartOne& p, uint32_t trigger)
{
  ActivateMessage msg;
  msg.data.caster = trigger;
  msg.data.target = trigger;
  msg.data.isSecondActivation = false;
  RawMessageData raw;
  raw.userId = 0;
  p.GetActionListener().OnActivate(raw, msg);
}
}

TEST_CASE("A trap's kill of a hosted NPC is taken on its trigger's firing, "
          "the trap itself counted where it stands",
          "[TrapKill][espm]")
{
  PartOne& p = GetPartOne();
  auto& draugr = Enter(p);
  const auto& children = p.worldState.activationChildsByActivationParent;
  REQUIRE(children.count(kPlate) == 1);
  REQUIRE(children.at(kPlate).count(kWall) == 1);
  // the body beside the wall and past the plate's own reach as evidence: the
  // wall's note alone can back it
  const NiPoint3 wall =
    p.worldState.GetFormAt<MpObjectReference>(kWall).GetPos();
  const NiPoint3 plate =
    p.worldState.GetFormAt<MpObjectReference>(kPlate).GetPos();
  const NiPoint3 body = wall + NiPoint3{ -400.f, 0.f, 0.f };
  REQUIRE((body - plate).Length() > 512.f);
  REQUIRE((body - wall).Length() < 512.f);

  // no evidence: refused, as playtest 47 read it (the refusal's line is
  // rate-limited per actor across the process, so its count is not read)
  ReportDeath(p, kDraugr, body);
  REQUIRE_FALSE(draugr.IsDead());

  // the plate fires itself: its wall is noted where it stands
  TriggerFired(p, kPlate);
  {
    LogCapture log;
    ReportDeath(p, kDraugr, body);
    REQUIRE(log.Count("a trap's evidence beside the body (54b15, ") == 1);
    REQUIRE(log.Count("the server records the death") == 1);
  }
  REQUIRE(draugr.IsDead());
  Leave(p);
}

TEST_CASE("A trap's evidence is kept the trap window, a player's activation "
          "the hazard window",
          "[TrapKill][espm]")
{
  PartOne& p = GetPartOne();
  auto& draugr = Enter(p);
  const NiPoint3 body = draugr.GetPos();
  auto& plate = p.worldState.GetFormAt<MpObjectReference>(kPlate);
  REQUIRE((body - plate.GetPos()).Length() < 512.f);
  const auto now = std::chrono::steady_clock::now();

  // a player's activation 10 s ago: past its 5 s, refused
  p.GetActionListener().NoteActivation(plate, now - std::chrono::seconds(10));
  ReportDeath(p, kDraugr, body);
  REQUIRE_FALSE(draugr.IsDead());
  // a trap's firing 40 s ago: past the trap window, refused
  p.GetActionListener().NoteTrapFiring(plate, now - std::chrono::seconds(40));
  ReportDeath(p, kDraugr, body);
  REQUIRE_FALSE(draugr.IsDead());
  // a trap's firing 20 s ago (the blade loop's 17.8 s in 47): taken
  p.GetActionListener().NoteTrapFiring(plate, now - std::chrono::seconds(20));
  ReportDeath(p, kDraugr, body);
  REQUIRE(draugr.IsDead());
  Leave(p);
}

TEST_CASE("A trap's kill of a hosted NPC is taken on a destruction record "
          "beside the body",
          "[TrapKill][espm]")
{
  PartOne& p = GetPartOne();
  auto& draugr = Enter(p);
  // the draugr burning by the lamp: its record moved beside it
  const NiPoint3 lamp =
    p.worldState.GetFormAt<MpObjectReference>(kLamp).GetPos();
  const NiPoint3 body = lamp + NiPoint3{ 120.f, 0.f, 0.f };
  draugr.SetPos(body);
  REQUIRE(p.worldState.CellOf(draugr) == std::optional<uint32_t>(kTemple));

  ReportDeath(p, kDraugr, body);
  REQUIRE_FALSE(draugr.IsDead());

  // the host's report of the lamp's first stage: recorded, then evidence
  {
    DestructionStageMessage msg;
    msg.refr = kLamp;
    msg.stage = 1;
    msg.destroyed = false;
    msg.served = false;
    RawMessageData raw;
    raw.userId = 0;
    LogCapture log;
    p.GetActionListener().OnDestructionStage(raw, msg);
    REQUIRE(log.Count("reports 9b0fb at stage 1, destroyed false: recorded "
                      "(the first report)") == 1);
  }
  {
    LogCapture log;
    ReportDeath(p, kDraugr, body);
    REQUIRE(log.Count("a trap's evidence beside the body (9b0fb, ") == 1);
  }
  REQUIRE(draugr.IsDead());
  Leave(p);
}

TEST_CASE("A trap's note backs a death in its own cell alone",
          "[TrapKill][espm]")
{
  PartOne& p = GetPartOne();
  auto& draugr = Enter(p);
  // the pool's note in the Sanctum, a Temple body at the same coordinates:
  // another cell, refused
  auto& pool = p.worldState.GetFormAt<MpObjectReference>(kPool);
  REQUIRE(p.worldState.CellOf(pool) == std::optional<uint32_t>(kSanctum));
  const auto now = std::chrono::steady_clock::now();
  p.GetActionListener().NoteActivation(pool, now, true);
  REQUIRE(p.GetActionListener()
            .ActivationSeenNear(pool, pool.GetPos(), now)
            .has_value());
  draugr.SetPos(pool.GetPos());
  REQUIRE(p.worldState.CellOf(draugr) == std::optional<uint32_t>(kTemple));
  REQUIRE_FALSE(p.GetActionListener()
                  .ActivationSeenNear(draugr, pool.GetPos(), now)
                  .has_value());
  ReportDeath(p, kDraugr, pool.GetPos());
  REQUIRE_FALSE(draugr.IsDead());
  Leave(p);
}

TEST_CASE("A host's linked report notes its target as a trap, kept the trap "
          "window (playtest 47's blades)",
          "[TrapKill][espm]")
{
  PartOne& p = GetPartOne();
  auto& draugr = Enter(p);
  p.worldState.GetFormAt<MpObjectReference>(kBoxTrigger);
  auto& blade = p.worldState.GetFormAt<MpObjectReference>(kBlade);
  const auto& children = p.worldState.activationChildsByActivationParent;
  REQUIRE(children.count(kBoxTrigger) == 1);
  REQUIRE(children.at(kBoxTrigger).count(kBlade) == 1);
  {
    ActivateMessage msg;
    msg.data.caster = kBoxTrigger;
    msg.data.target = kBlade;
    msg.data.isSecondActivation = false;
    RawMessageData raw;
    raw.userId = 0;
    p.GetActionListener().OnActivate(raw, msg);
  }
  const NiPoint3 body = blade.GetPos() + NiPoint3{ 100.f, 0.f, 0.f };
  draugr.SetPos(body);
  const auto now = std::chrono::steady_clock::now();
  // 15 s on (47's blade kill came 17.8 s after its trigger's report): still
  // within the trap window, where a player's activation would have lapsed
  const auto at15 = p.GetActionListener().ActivationSeenNear(
    draugr, body, now + std::chrono::seconds(15));
  REQUIRE(at15.has_value());
  REQUIRE(at15->trap);
  REQUIRE(at15->refId == kBlade);
  // past the trap window: gone
  REQUIRE_FALSE(
    p.GetActionListener()
      .ActivationSeenNear(draugr, body, now + std::chrono::seconds(35))
      .has_value());
  // the kill itself: taken on the blade's note
  {
    LogCapture log;
    ReportDeath(p, kDraugr, body);
    REQUIRE(log.Count("a trap's evidence beside the body (3ac33, ") == 1);
  }
  REQUIRE(draugr.IsDead());
  Leave(p);
}

TEST_CASE("A trigger's firing notes its activation children two levels down",
          "[TrapKill][espm]")
{
  PartOne& p = GetPartOne();
  // the platform registers under the linker and the linker under the wire as
  // each loads (its XAPR); nothing else of the mine is loaded, so the
  // platform is the one level-two child
  auto& wire = p.worldState.GetFormAt<MpObjectReference>(kWire);
  p.worldState.GetFormAt<MpObjectReference>(kLinker);
  auto& platform = p.worldState.GetFormAt<MpObjectReference>(kPlatform);
  const auto& children = p.worldState.activationChildsByActivationParent;
  REQUIRE(children.count(kWire) == 1);
  REQUIRE(children.at(kWire).count(kLinker) == 1);
  REQUIRE(children.count(kLinker) == 1);
  REQUIRE(children.at(kLinker).count(kPlatform) == 1);
  const auto now = std::chrono::steady_clock::now();
  p.GetActionListener().NoteTrapFiring(wire, now);
  const auto seen =
    p.GetActionListener().ActivationSeenNear(platform, platform.GetPos(), now);
  REQUIRE(seen.has_value());
  REQUIRE(seen->trap);
  REQUIRE(seen->refId == kPlatform);
}
