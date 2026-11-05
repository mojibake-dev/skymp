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

// thuum docs/verbs/traps.md (M4): the cell's host's linked activations. Bleak
// Falls Temple 0x371de: the spike wall 0x54b15's one activation parent is the
// plate 0x567f2 (unit/ActivateParentTest), the portcullis 0x3b9f1's name the
// lever 0x6a9e2 (lab/esm.py 2026-11-01, XAPR e2a90600). The plate stands at
// (8589, -5338, -2054), the portcullis at (4992, -1408, -768). The portcullis
// is an ACTI (NorPortcullisLarge01 0x3e726), not a DOOR: its rise is each
// engine's script on the relayed activation, and the server keeps no open
// state for it (pipeline 4392 read IsOpen false after the lever's report).
namespace {
constexpr uint32_t kHostActor = 0xff000fe1;
constexpr uint32_t kGuestActor = 0xff000fe2;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPlate = 0x567f2;
constexpr uint32_t kWall = 0x54b15;
constexpr uint32_t kLever = 0x6a9e2;
constexpr uint32_t kPortcullis = 0x3b9f1;
// the puzzle room (ADR-059 decision 2): the lever LeverPushAnimatingNoPlayer
// 0xaa8c3 carries defaultPillarPuzzleLever, whose properties name the dart
// trap 0x3720f (refActOnFailure02) and the marker 0x3e728
// (puzzleDoorActivator), the marker's activation parents the portcullis
// (XAPR); the marker is a STAT reference the server never loads
constexpr uint32_t kPuzzleLever = 0xaa8c3;
constexpr uint32_t kDart = 0x3720f;
constexpr uint32_t kMarker = 0x3e728;
// the first pillar NorDefaultPuzzlePillar01 0xaa8cb at (5656, -896, -768),
// defaultPuzzlePillarScript on the REFR and on the base
constexpr uint32_t kPillar = 0xaa8cb;
const NiPoint3 kByThePlate{ 8589.f, -5438.f, -2054.f };
const NiPoint3 kByThePortcullis{ 4992.f, -1508.f, -768.f };
const NiPoint3 kByThePuzzleLever{ 5002.f, -1003.f, -754.f };
const NiPoint3 kByThePillar{ 5500.f, -896.f, -768.f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// `user` hosts `cell` (ActorDeathTest's shape): any earlier host's grace
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

// both players at `pos`, loaded references around them subscribed
void Stand(PartOne& p, const NiPoint3& pos)
{
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(pos);
    actor.ForceSubscriptionsUpdate();
  }
}

void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kByThePlate, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kByThePlate, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  // the targets loaded (their activation parents fill the table at Init)
  p.worldState.GetFormAt<MpObjectReference>(kWall);
  p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  Stand(p, kByThePlate);
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

// a game's report of its engine's linked activation (activationService)
void Linked(PartOne& p, Networking::UserId user, uint32_t caster,
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
// (EffectConditionsTest's shape), restored at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("trap-relay-test", sink);
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

TEST_CASE("The host's linked activation is recorded through the reference's "
          "own path and relayed to the cell's other game",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the plate fires the spike wall: relayed to the guest, not the host
  Linked(p, 0, kPlate, kWall);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kWall);
  REQUIRE(RelayedTo(p, 1)[0].caster == kPlate);
  REQUIRE(RelayedTo(p, 0).empty());
  // the lever's activation of the portcullis (an ACTI: the rise is the
  // engines' own on the relay) reaches the guest with the lever as caster
  Stand(p, kByThePortcullis);
  p.Messages().clear();
  Linked(p, 0, kLever, kPortcullis);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kPortcullis);
  REQUIRE(RelayedTo(p, 1)[0].caster == kLever);
  Leave(p);
}

TEST_CASE("A linked activation from a guest, an unlinked pair or past the "
          "budget is refused",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the guest does not host the cell
  Linked(p, 1, kPlate, kWall);
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());
  // the plate is no activation parent of the portcullis
  Linked(p, 0, kPlate, kPortcullis);
  REQUIRE(RelayedTo(p, 1).empty());
  // eight in a second pass, the ninth is dropped
  for (int i = 0; i < 8; ++i) {
    Linked(p, 0, kPlate, kWall);
  }
  REQUIRE(RelayedTo(p, 1).size() == 8);
  Linked(p, 0, kPlate, kWall);
  REQUIRE(RelayedTo(p, 1).size() == 8);
  Leave(p);
}

TEST_CASE("The puzzle lever's script properties are links, and a marker the "
          "server never loads is a caster by its id (ADR-059 decision 2)",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the lever's own Init fills its links; the dart is an ACTI the loader takes
  p.worldState.GetFormAt<MpObjectReference>(kPuzzleLever);
  p.worldState.GetFormAt<MpObjectReference>(kDart);
  Stand(p, kByThePuzzleLever);
  p.Messages().clear();
  // the failure's dart trap, linked by refActOnFailure02
  Linked(p, 0, kPuzzleLever, kDart);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kDart);
  REQUIRE(RelayedTo(p, 1)[0].caster == kPuzzleLever);
  // the marker as a target is no form of the server's: refused, no relay (its
  // consequence is the next report)
  p.Messages().clear();
  Linked(p, 0, kPuzzleLever, kMarker);
  REQUIRE(RelayedTo(p, 1).empty());
  // the marker as the caster of the portcullis's activation: the portcullis's
  // XAPR names it, so the link holds by id although the marker never loads
  Stand(p, kByThePortcullis);
  p.Messages().clear();
  Linked(p, 0, kMarker, kPortcullis);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kPortcullis);
  REQUIRE(RelayedTo(p, 1)[0].caster == kMarker);
  // a stranger's id that is no reference at all falls to the hosted caster
  // rule's throw (PartOne_ActivateTest's "Activate with bad hoster")
  p.Messages().clear();
  REQUIRE_THROWS_WITH(Linked(p, 0, 0x12345, kPortcullis),
                      Catch::Matchers::ContainsSubstring("Bad hoster"));
  REQUIRE(RelayedTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("A host's linked activation of an actor is the actor's "
          "simulator's: neither judged nor relayed (ADR-068)",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the ambush trigger defaultActivateSelfTRIG 0x38098 at (7921, -4613, -1795)
  // and two of its ACHR children (their XAPR name it, lab/esm.py 2026-11-05):
  // 0x380b3 at (8657, -5215, -2055) and 0x380b4 at (9016, -4725, -2054)
  constexpr uint32_t kAmbushTrigger = 0x38098;
  constexpr uint32_t kDraugr = 0x380b3;
  constexpr uint32_t kDraugrUnloaded = 0x380b4;
  p.worldState.GetFormAt<MpObjectReference>(kAmbushTrigger);
  p.Messages().clear();
  // an actor the server never loaded, judged by its game-file record (ACHR):
  // reported while the fresh world loads no NPC (npcEnabled false), so the
  // report's own lookup cannot load it, whatever its record's flags
  {
    LogCapture log;
    REQUIRE_NOTHROW(Linked(p, 0, kAmbushTrigger, kDraugrUnloaded));
    REQUIRE(log.Count("[verb:traps] ff000fe1 reports 38098 activating actor "
                      "380b4: the actor's simulator's (the NPC stream "
                      "carries it), not relayed") == 1);
    REQUIRE(log.Count("E_TRAP_") == 0);
  }
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kDraugrUnloaded) == nullptr);
  REQUIRE(RelayedTo(p, 1).empty());
  REQUIRE(RelayedTo(p, 0).empty());
  // a loaded actor, through the loaded reference's own path: NPCs on, the
  // draugr loaded by its id (NpcFateTest's shape), simulated by the reporter,
  // which hosts the temple
  p.worldState.npcEnabled = true;
  auto& draugr = p.worldState.GetFormAt<MpActor>(kDraugr);
  REQUIRE(p.worldState.HostOf(draugr) ==
          &p.worldState.GetFormAt<MpActor>(kHostActor));
  p.Messages().clear();
  {
    LogCapture log;
    REQUIRE_NOTHROW(Linked(p, 0, kAmbushTrigger, kDraugr));
    REQUIRE(log.Count("[verb:traps] ff000fe1 reports 38098 activating actor "
                      "380b3: the actor's simulator's (the NPC stream "
                      "carries it), not relayed") == 1);
    REQUIRE(log.Count("E_TRAP_") == 0);
  }
  REQUIRE(RelayedTo(p, 1).empty());
  REQUIRE(RelayedTo(p, 0).empty());
  // the plate's spike wall still relays beside it
  Linked(p, 0, kPlate, kWall);
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kWall);
  Leave(p);
}

TEST_CASE("A host's linked activation of an actor another seat simulates "
          "goes to that seat's game alone (ADR-068)",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the ambush trigger and its loaded draugr, as the case above has them
  constexpr uint32_t kAmbushTrigger = 0x38098;
  constexpr uint32_t kDraugr = 0x380b3;
  p.worldState.GetFormAt<MpObjectReference>(kAmbushTrigger);
  p.worldState.npcEnabled = true;
  auto& draugr = p.worldState.GetFormAt<MpActor>(kDraugr);
  auto& guest = p.worldState.GetFormAt<MpActor>(kGuestActor);
  // before any claim the cell's host simulates the draugr, so a guest's
  // report of the pair would wake the host's own actor: refused, since the
  // guest does not host the cell the trigger stands in
  REQUIRE(p.worldState.HostOf(draugr) ==
          &p.worldState.GetFormAt<MpActor>(kHostActor));
  {
    LogCapture log;
    REQUIRE_NOTHROW(Linked(p, 1, kAmbushTrigger, kDraugr));
    REQUIRE(log.Count("[verb:traps] E_TRAP_NOT_HOST: ff000fe2 reports 38098 "
                      "activating actor 380b3, its trigger in a cell it does "
                      "not host") == 1);
    REQUIRE(log.Count("relayed to it") == 0);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());
  // the guest loads the temple and claims the draugr (ADR-052, CellHostTest's
  // claim): its game simulates the actor in the host's cell. The claim is the
  // per-actor row this branch has; a pass-margin follow and a thrall's
  // commander (ADR-060, once its row lands) are the same shape to this rule,
  // another seat in HostOf's answer (an exterior ambush's actor in a cell
  // another seat hosts never reaches the server: the client reports only for
  // a target in a cell its seat hosts)
  {
    RawMessageData raw;
    raw.userId = 1;
    CellLoadedMessage loaded;
    loaded.cell = kTemple;
    loaded.loaded = true;
    p.GetActionListener().OnCellLoaded(raw, loaded);
  }
  guest.SetPos(draugr.GetPos() + NiPoint3{ 80.f, 0.f, 0.f });
  {
    static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
    RawMessageData raw;
    raw.userId = 1;
    raw.unparsed = unparsed;
    raw.unparsedLength = sizeof(unparsed);
    ActorClaimMessage claim;
    claim.idx = draugr.GetIdx();
    claim.claim = true;
    p.GetActionListener().OnActorClaim(raw, claim);
  }
  REQUIRE(p.worldState.HostOf(draugr) == &guest);
  p.Messages().clear();
  // the host's engine fired the trigger: the activation goes to the guest's
  // game alone, whose engine runs the draugr, with the trigger as its caster
  {
    LogCapture log;
    REQUIRE_NOTHROW(Linked(p, 0, kAmbushTrigger, kDraugr));
    REQUIRE(log.Count("[verb:traps] ff000fe1 reports 38098 activating actor "
                      "380b3: its simulator ff000fe2 is another seat's, "
                      "relayed to it") == 1);
    REQUIRE(log.Count("the actor's simulator's") == 0);
    REQUIRE(log.Count("E_TRAP_") == 0);
  }
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kDraugr);
  REQUIRE(RelayedTo(p, 1)[0].caster == kAmbushTrigger);
  REQUIRE(RelayedTo(p, 0).empty());
  // the plate is no activation parent of the draugr (its record's XAPR names
  // the trigger alone): refused, nothing relayed
  p.worldState.GetFormAt<MpObjectReference>(kPlate);
  p.Messages().clear();
  {
    LogCapture log;
    REQUIRE_NOTHROW(Linked(p, 0, kPlate, kDraugr));
    REQUIRE(log.Count("[verb:traps] E_TRAP_UNLINKED: ff000fe1 reports 567f2 "
                      "activating actor 380b3, whose activation parents do "
                      "not name it") == 1);
    REQUIRE(log.Count("relayed to it") == 0);
  }
  REQUIRE(RelayedTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("A player's activation of a scripted world object goes to the "
          "cell's other games with the player as the activator, and an "
          "echo of it is quiet (ADR-059 decision 2)",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  p.worldState.GetFormAt<MpObjectReference>(kPillar);
  Stand(p, kByThePillar);
  p.Messages().clear();
  // the guest (user 1) turns the pillar: the host (user 0) gets the relay
  Linked(p, 1, 0x14, kPillar);
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kPillar);
  REQUIRE(RelayedTo(p, 0)[0].caster == kGuestActor);
  REQUIRE(RelayedTo(p, 1).empty());
  // the host's engine echoes it as the figure's activation: taken quietly
  p.Messages().clear();
  Linked(p, 0, kGuestActor, kPillar);
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());
  // the host's own turn goes to the guest's engine the same way
  p.Messages().clear();
  Linked(p, 0, 0x14, kPillar);
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].target == kPillar);
  REQUIRE(RelayedTo(p, 1)[0].caster == kHostActor);
  // and the guest's echo of the host's figure is quiet too
  p.Messages().clear();
  Linked(p, 1, kHostActor, kPillar);
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());
  Leave(p);
}

// thuum D-S3 phase 2 (any seat triggers, Eli's requirement): Bleak Falls
// Temple's trigger box 0x86018 (TrapTriggerBox 0x7617a) at (8211.7, -6621.7,
// -2112.0), whose own VMAD sets TrapTriggerBase {TriggerType 3,
// playerOnlyTrigger true}, and one of the swinging blades it fires, 0x3ac33
// (TrapBladeSwinging01 0x1cdd0) at (6464, -7168, -2304), whose XAPR names the
// box and the pull chain 0xfda45 (lab/esm.py on Skyrim.esm, 2026-11-05; the
// census's row 4). The plate 0x567f2 sets no playerOnlyTrigger (its record
// has no VMAD, its base's PressurePlate none of the property).
TEST_CASE("A player-only trigger's linked activation is taken from a guest "
          "whose game has the cell loaded, and every other pair stays the "
          "host's",
          "[Traps][espm]")
{
  constexpr uint32_t kPlayerOnlyBox = 0x86018;
  constexpr uint32_t kBlade = 0x3ac33;
  const NiPoint3 kByTheBlade{ 6464.f, -7068.f, -2304.f };
  PartOne& p = GetPartOne();
  Enter(p);
  p.worldState.GetFormAt<MpObjectReference>(kPlayerOnlyBox);
  p.worldState.GetFormAt<MpObjectReference>(kBlade);
  Stand(p, kByTheBlade);
  p.Messages().clear();

  // the guest's game has not reported the temple loaded: refused as before
  {
    LogCapture log;
    Linked(p, 1, kPlayerOnlyBox, kBlade);
    REQUIRE(log.Count("[verb:traps] E_TRAP_NOT_HOST: ff000fe2 reports 86018 "
                      "activating 3ac33 in cell 371de, which it does not "
                      "host") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());

  // the guest's game reports the temple loaded: the box's pair is taken from
  // it, recorded through the blade's own path and relayed to the host
  {
    RawMessageData raw;
    raw.userId = 1;
    CellLoadedMessage loaded;
    loaded.cell = kTemple;
    loaded.loaded = true;
    p.GetActionListener().OnCellLoaded(raw, loaded);
  }
  REQUIRE(p.GetCellHost(kTemple).player == kHostActor);
  {
    LogCapture log;
    Linked(p, 1, kPlayerOnlyBox, kBlade);
    REQUIRE(log.Count("[verb:traps] 86018 activates 3ac33 in cell 371de (a "
                      "player-only trigger, ff000fe2 reports with the cell "
                      "loaded): relayed to 1 game(s)") == 1);
    REQUIRE(log.Count("E_TRAP_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kBlade);
  REQUIRE(RelayedTo(p, 0)[0].caster == kPlayerOnlyBox);
  REQUIRE(RelayedTo(p, 1).empty());

  // the plate sets no playerOnlyTrigger: the guest's report of its spike
  // wall stays refused with the cell loaded
  Stand(p, kByThePlate);
  p.Messages().clear();
  {
    LogCapture log;
    Linked(p, 1, kPlate, kWall);
    REQUIRE(log.Count("[verb:traps] E_TRAP_NOT_HOST: ff000fe2 reports 567f2 "
                      "activating 54b15 in cell 371de, which it does not "
                      "host") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());

  // the host's own report of the box's pair is the cell path's, its line
  // unchanged
  Stand(p, kByTheBlade);
  p.Messages().clear();
  {
    LogCapture log;
    Linked(p, 0, kPlayerOnlyBox, kBlade);
    REQUIRE(log.Count("[verb:traps] 86018 activates 3ac33 in cell 371de (its "
                      "host ff000fe1 reports): relayed to 1 game(s)") == 1);
  }
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].caster == kPlayerOnlyBox);
  Leave(p);
}
