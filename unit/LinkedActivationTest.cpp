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

// thuum docs/DECISIONS.md ADR-068 D-W1 (Eli 2026-11-06; docs/research/
// fifty-fixes-2026-11-07.md section 1(c)): a trap trigger's firing is its
// presser's owner's, so the server takes the trigger's own activation and
// its first links from the cell's host as before and from a game with the
// cell loaded one of whose actors (its player, or an actor it simulates)
// stands within the trigger's activation reach plus the firing's slack
// (wire-rules trigger_entry's firing_source); D-S3 phase 2's player-only
// read retired into it. The stage is TrapRelayTest's: Bleak Falls Temple
// 0x371de, the plate 0x567f2 at (8589, -5338, -2054) and the spike wall
// 0x54b15 its activation parent names (unit/ActivateParentTest), the trigger
// box 0x86018 (TrapTriggerBox, its own VMAD playerOnlyTrigger true) at
// (8211.7, -6621.7, -2112.0) and the swinging blade 0x3ac33 at (6464, -7168,
// -2304) whose XAPR names it, the portcullis 0x3b9f1 at (4992, -1408, -768),
// some five thousand units off the plate, whose XAPR names the puzzle marker
// 0x3e728 the server never loads, and the draugr 0x380b3 at (8657, -5215,
// -2055) beside the plate (lab/esm.py on Skyrim.esm, 2026-11-01 and 11-05).
namespace {
constexpr uint32_t kHostActor = 0xff000ff1;
constexpr uint32_t kGuestActor = 0xff000ff2;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPlate = 0x567f2;
constexpr uint32_t kWall = 0x54b15;
constexpr uint32_t kBox = 0x86018;
constexpr uint32_t kBlade = 0x3ac33;
constexpr uint32_t kPortcullis = 0x3b9f1;
constexpr uint32_t kMarker = 0x3e728;
constexpr uint32_t kDraugr = 0x380b3;
const NiPoint3 kByThePlate{ 8589.f, -5438.f, -2054.f };
const NiPoint3 kByTheBox{ 8211.7f, -6571.7f, -2112.f };
const NiPoint3 kByTheBlade{ 6464.f, -7068.f, -2304.f };
const NiPoint3 kByThePortcullis{ 4992.f, -1508.f, -768.f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// `user`'s game reports `cell` loaded
void Loaded(PartOne& p, Networking::UserId user, uint32_t cell)
{
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage loaded;
  loaded.cell = cell;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
}

// `user` hosts `cell` (TrapRelayTest's shape): any earlier host's grace
// passes first, then the loaded report grants the cell
void Host(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  Loaded(p, user, cell);
  p.TickCellHosts(past);
}

// one actor at `pos`, the loaded references around it subscribed
void Place(PartOne& p, uint32_t id, const NiPoint3& pos)
{
  auto& actor = p.worldState.GetFormAt<MpActor>(id);
  actor.SetPos(pos);
  actor.ForceSubscriptionsUpdate();
}

// both players by the plate, the host's game hosting the temple; the
// guest's game has not reported it loaded
void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kByThePlate, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kByThePlate, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  // the triggers and the targets loaded (the targets' activation parents
  // fill the table at Init)
  for (uint32_t id : { kPlate, kWall, kBox, kBlade, kPortcullis }) {
    p.worldState.GetFormAt<MpObjectReference>(id);
  }
  Place(p, kHostActor, kByThePlate);
  Place(p, kGuestActor, kByThePlate);
  Host(p, 0, kTemple);
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

// a game's report of its engine's activation of `target` by `caster`
// (activationService): a linked pair, or a trigger's own activation when
// the two are the same
void Activation(PartOne& p, Networking::UserId user, uint32_t caster,
                uint32_t target)
{
  ActivateMessage msg;
  msg.data.caster = caster;
  msg.data.target = target;
  msg.data.isSecondActivation = false;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnActivate(raw, msg);
}

std::vector<RelayedActivationMessage> RelayedTo(PartOne& p,
                                                Networking::UserId user)
{
  std::vector<RelayedActivationMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<RelayedActivationMessage*>(m.message.get());
    if (m.userId == user && typed) {
      out.push_back(*typed);
    }
  }
  return out;
}

// the default logger's lines at info and above while the guard lives
// (TrapRelayTest's shape), restored at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger =
      std::make_shared<spdlog::logger>("linked-activation-test", sink);
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

TEST_CASE("A guest's firing in the host's cell is taken from its game with "
          "the cell loaded and relayed to the host (D-W1)",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);

  // the guest's game has not reported the temple loaded: neither the host
  // nor a loader
  {
    LogCapture log;
    Activation(p, 1, kPlate, kWall);
    REQUIRE(log.Count("[verb:traps] E_TRAP_NOT_HOST: ff000ff2 reports 567f2 "
                      "activating 54b15 in cell 371de, which it neither "
                      "hosts nor has loaded") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());

  // it reports the temple loaded and presses the plate: the root is taken
  // from it and relayed to the host, whose engine plays it
  Loaded(p, 1, kTemple);
  REQUIRE(p.GetCellHost(kTemple).player == kHostActor);
  {
    LogCapture log;
    Activation(p, 1, kPlate, kWall);
    REQUIRE(log.Count("[verb:traps] 567f2 activates 54b15 in cell 371de "
                      "(ff000ff2 reports with the cell loaded, its firing): "
                      "relayed to 1 game(s)") == 1);
    REQUIRE(log.Count("E_TRAP_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kWall);
  REQUIRE(RelayedTo(p, 0)[0].caster == kPlate);
  REQUIRE(RelayedTo(p, 1).empty());

  // the player-only box the same way, its blade some 1,800 units off it:
  // the reach is the box's, its presser's owner's word (D-S3 phase 2's read
  // retired into the rule)
  Place(p, kGuestActor, kByTheBox);
  Place(p, kHostActor, kByTheBlade);
  p.Messages().clear();
  {
    LogCapture log;
    Activation(p, 1, kBox, kBlade);
    REQUIRE(log.Count("[verb:traps] 86018 activates 3ac33 in cell 371de "
                      "(ff000ff2 reports with the cell loaded, its firing): "
                      "relayed to 1 game(s)") == 1);
    REQUIRE(log.Count("E_TRAP_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kBlade);
  REQUIRE(RelayedTo(p, 0)[0].caster == kBox);

  // a loader's unlinked pair is the link's refusal (the plate is no
  // activation parent of the portcullis)
  Place(p, kGuestActor, kByThePlate);
  Place(p, kHostActor, kByThePlate);
  p.Messages().clear();
  {
    LogCapture log;
    Activation(p, 1, kPlate, kPortcullis);
    REQUIRE(log.Count("[verb:traps] E_TRAP_UNLINKED: ff000ff2 reports 567f2 "
                      "activating 3b9f1, whose activation parents do not "
                      "name it") == 1);
    REQUIRE(log.Count("E_TRAP_NOT_HOST") == 0);
    REQUIRE(log.Count("E_TRAP_FAR") == 0);
  }
  REQUIRE(RelayedTo(p, 0).empty());

  // the host's own report keeps its line and goes to the guest
  {
    LogCapture log;
    Activation(p, 0, kPlate, kWall);
    REQUIRE(log.Count("[verb:traps] 567f2 activates 54b15 in cell 371de (its "
                      "host ff000ff1 reports): relayed to 1 game(s)") == 1);
  }
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].caster == kPlate);
  Leave(p);
}

TEST_CASE("A loader none of whose actors stands by the trigger is refused "
          "E_TRAP_FAR, and an actor it simulates there counts (D-W1)",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Loaded(p, 1, kTemple);
  auto& guest = p.worldState.GetFormAt<MpActor>(kGuestActor);

  // the guest's player across the temple, by the portcullis
  Place(p, kGuestActor, kByThePortcullis);
  p.Messages().clear();
  {
    LogCapture log;
    Activation(p, 1, kPlate, kWall);
    REQUIRE(log.Count("[verb:traps] E_TRAP_FAR: ff000ff2 reports 567f2 "
                      "activating 54b15 in cell 371de, its nearest actor ") ==
            1);
    REQUIRE(log.Count("activates 54b15") == 0);
  }
  REQUIRE(RelayedTo(p, 0).empty());

  // a caster the server never loads (the marker) stands nowhere it can
  // measure: a loader's word on it is refused, the host's taken
  {
    LogCapture log;
    Activation(p, 1, kMarker, kPortcullis);
    REQUIRE(log.Count("[verb:traps] E_TRAP_FAR: ff000ff2 reports 3e728 "
                      "activating 3b9f1 in cell 371de, none of its actors "
                      "stands where the trigger does") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  Activation(p, 0, kMarker, kPortcullis);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].caster == kMarker);

  // the guest claims the draugr by the plate (ADR-052, TrapRelayTest's
  // claim) and walks back across the temple: its game simulates the
  // draugr, which stands by the plate, so its press of the plate is the
  // presser's owner's
  p.worldState.npcEnabled = true;
  auto& draugr = p.worldState.GetFormAt<MpActor>(kDraugr);
  draugr.ForceSubscriptionsUpdate();
  guest.SetPos(draugr.GetPos() + NiPoint3{ 80.f, 0.f, 0.f });
  const auto setClaim = [&](bool on) {
    static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
    RawMessageData raw;
    raw.userId = 1;
    raw.unparsed = unparsed;
    raw.unparsedLength = sizeof(unparsed);
    ActorClaimMessage msg;
    msg.idx = draugr.GetIdx();
    msg.claim = on;
    p.GetActionListener().OnActorClaim(raw, msg);
  };
  setClaim(true);
  REQUIRE(p.worldState.HostOf(draugr) == &guest);
  Place(p, kGuestActor, kByThePortcullis);
  REQUIRE(p.worldState.HostOf(draugr) == &guest);
  p.Messages().clear();
  {
    LogCapture log;
    Activation(p, 1, kPlate, kWall);
    REQUIRE(log.Count("[verb:traps] 567f2 activates 54b15 in cell 371de "
                      "(ff000ff2 reports with the cell loaded, its firing): "
                      "relayed to 1 game(s)") == 1);
    REQUIRE(log.Count("E_TRAP_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kWall);

  // released, the draugr is the host's again: the guest's word is far
  setClaim(false);
  REQUIRE(p.worldState.HostOf(draugr) ==
          &p.worldState.GetFormAt<MpActor>(kHostActor));
  p.Messages().clear();
  {
    LogCapture log;
    Activation(p, 1, kPlate, kWall);
    REQUIRE(log.Count("[verb:traps] E_TRAP_FAR: ff000ff2 reports 567f2 "
                      "activating 54b15 in cell 371de, its nearest actor ") ==
            1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  Leave(p);
}

TEST_CASE("A trigger's own activation is noted from its firing's owner, the "
          "host or a loader by the trigger, and refused by name from "
          "another game (D-W1)",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  auto& plate = p.worldState.GetFormAt<MpObjectReference>(kPlate);
  const auto noted = [&] {
    const auto seen = p.GetActionListener().ActivationSeenNear(
      plate, plate.GetPos(), std::chrono::steady_clock::now());
    return seen.has_value() && seen->trap;
  };

  // the guest's game has not loaded the temple: dropped by name, nothing
  // noted
  {
    LogCapture log;
    Activation(p, 1, kPlate, kPlate);
    REQUIRE(log.Count("[verb:npc-fate] E_TRAP_NOT_HOST: ff000ff2 reports "
                      "trigger 567f2 firing itself in cell 371de, which it "
                      "neither hosts nor has loaded; dropped") == 1);
    REQUIRE(log.Count("fired itself") == 0);
  }
  REQUIRE_FALSE(noted());

  // loaded, but its player across the temple: far, nothing noted
  Loaded(p, 1, kTemple);
  Place(p, kGuestActor, kByThePortcullis);
  {
    LogCapture log;
    Activation(p, 1, kPlate, kPlate);
    REQUIRE(
      log.Count("[verb:npc-fate] E_TRAP_FAR: ff000ff2 reports trigger "
                "567f2 firing itself in cell 371de, its nearest actor ") == 1);
    REQUIRE(log.Count("fired itself") == 0);
  }
  REQUIRE_FALSE(noted());

  // by the plate: its press is its own firing, noted as a hazard death's
  // evidence
  Place(p, kGuestActor, kByThePlate);
  {
    LogCapture log;
    Activation(p, 1, kPlate, kPlate);
    REQUIRE(log.Count("[verb:npc-fate] trigger 567f2 fired itself in cell "
                      "371de (ff000ff2 reports with the cell loaded, its "
                      "firing): noted as a hazard death's evidence") == 1);
    REQUIRE(log.Count("E_TRAP_") == 0);
  }
  REQUIRE(noted());

  // the host's own report keeps its line; a firing is noted, never relayed
  {
    LogCapture log;
    Activation(p, 0, kPlate, kPlate);
    REQUIRE(log.Count("[verb:npc-fate] trigger 567f2 fired itself in cell "
                      "371de (its host ff000ff1 reports): noted as a hazard "
                      "death's evidence") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());
  Leave(p);
}

// the ambush trigger defaultActivateSelfTRIG 0x38098 at (7921, -4613, -1795)
// and its ACHR child 0x380b3, whose XAPR names it (TrapRelayTest's): the
// review of m4-train-50, note 4. A guest's press of a trigger whose first
// link is an actor the host simulates goes to the host's game, whose engine
// wakes the actor, when the guest's game has the cell loaded and its player
// stands by the trigger; one across the temple is refused E_TRAP_FAR
TEST_CASE("A guest's firing whose first link is an actor the host simulates "
          "is relayed to the host from a loader by the trigger (D-W1)",
          "[Traps][espm]")
{
  constexpr uint32_t kAmbushTrigger = 0x38098;
  const NiPoint3 kByTheAmbush{ 7921.f, -4713.f, -1795.f };
  PartOne& p = GetPartOne();
  Enter(p);
  p.worldState.GetFormAt<MpObjectReference>(kAmbushTrigger);
  p.worldState.npcEnabled = true;
  auto& draugr = p.worldState.GetFormAt<MpActor>(kDraugr);
  REQUIRE(p.worldState.HostOf(draugr) ==
          &p.worldState.GetFormAt<MpActor>(kHostActor));
  Loaded(p, 1, kTemple);

  // the guest's player across the temple: far, nothing relayed
  Place(p, kGuestActor, kByThePortcullis);
  p.Messages().clear();
  {
    LogCapture log;
    REQUIRE_NOTHROW(Activation(p, 1, kAmbushTrigger, kDraugr));
    REQUIRE(log.Count("[verb:traps] E_TRAP_FAR: ff000ff2 reports 38098 "
                      "activating actor 380b3, its trigger in cell 371de, its "
                      "nearest actor ") == 1);
    REQUIRE(log.Count("relayed to it") == 0);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());

  // by the trigger: its press is the presser's owner's, the waking goes to
  // the draugr's simulator, the host, alone
  Place(p, kGuestActor, kByTheAmbush);
  p.Messages().clear();
  {
    LogCapture log;
    REQUIRE_NOTHROW(Activation(p, 1, kAmbushTrigger, kDraugr));
    REQUIRE(log.Count("[verb:traps] ff000ff2 reports 38098 activating actor "
                      "380b3: its simulator ff000ff1 is another seat's, "
                      "relayed to it") == 1);
    REQUIRE(log.Count("E_TRAP_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kDraugr);
  REQUIRE(RelayedTo(p, 0)[0].caster == kAmbushTrigger);
  REQUIRE(RelayedTo(p, 1).empty());

  // the plate is no activation parent of the draugr: the XAPR check still
  // refuses a loader's pair by the plate
  Place(p, kGuestActor, kByThePlate);
  p.Messages().clear();
  {
    LogCapture log;
    REQUIRE_NOTHROW(Activation(p, 1, kPlate, kDraugr));
    REQUIRE(log.Count("[verb:traps] E_TRAP_UNLINKED: ff000ff2 reports 567f2 "
                      "activating actor 380b3, whose activation parents do "
                      "not name it") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  Leave(p);
}
