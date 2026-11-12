#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "libespm/ObjectBounds.h"
#include "libespm/REFR.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum D-W1 and D-C2 (playtest 50; docs/research/fifty-fixes-2026-11-07.md
// 1(c)): a guest's report of its own presser's entry into a world trigger,
// relayed to the cell's host alone. Bleak Falls Temple 0x371de's entrance
// web 0x37281 (FXtriggerAmbFX on FXcobwebRoundWall01ACT 0x1eeb9, an ACTI) at
// (-1495.9, 1020.1, -54.4), its navmesh floor at -15.1
// (lab/.cache/drafts/fifty/trigger-entry-static.md section 6); the draugr
// 0x380b3 an ACHR of the Temple (TrapRelayTest's). The rule is wire-rules
// trigger_entry's entry (S1's crate tests hold its every branch, the order
// among them and E_ENTRY_NO_HOST, which a T0 world hosts at the first
// loaded report); here the handler's facts and its relay.
namespace {
constexpr uint32_t kHostActor = 0xff000fe1;
constexpr uint32_t kGuestActor = 0xff000fe2;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kWeb = 0x37281;
constexpr uint32_t kDraugr = 0x380b3;
const NiPoint3 kInTheWeb{ -1495.9f, 1020.1f, -15.1f };
const NiPoint3 kFarFromTheWeb{ -1495.9f, 3020.1f, -15.1f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

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

void Stand(PartOne& p, uint32_t actorId, const NiPoint3& pos)
{
  auto& actor = p.worldState.GetFormAt<MpActor>(actorId);
  actor.SetPos(pos);
  actor.ForceSubscriptionsUpdate();
}

// both players in the web, the host's game hosting the Temple; the guest's
// game has not reported the Temple loaded yet
void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kInTheWeb, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kInTheWeb, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  p.worldState.GetFormAt<MpObjectReference>(kWeb);
  Stand(p, kHostActor, kInTheWeb);
  Stand(p, kGuestActor, kInTheWeb);
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

// a game's report of its presser's entry (activationService's, D-C2)
void Entry(PartOne& p, Networking::UserId user, uint64_t presser,
           uint64_t trigger, uint32_t entry)
{
  ActivateMessage msg;
  msg.data.caster = presser;
  msg.data.target = trigger;
  msg.data.isSecondActivation = false;
  msg.data.entry = entry;
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

size_t SentTo(PartOne& p, Networking::UserId user)
{
  size_t n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == user) {
      ++n;
    }
  }
  return n;
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
    auto logger = std::make_shared<spdlog::logger>("trigger-entry-test", sink);
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

TEST_CASE("A guest's entry into a world trigger goes to the cell's host "
          "alone, with the guest's figure as the presser (D-C2)",
          "[TriggerEntry][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Loaded(p, 1, kTemple);
  p.Messages().clear();
  {
    LogCapture log;
    Entry(p, 1, 0x14, kWeb, 1);
    REQUIRE(log.Count("[verb:traps] ff000fe2 reports ff000fe2 entering "
                      "37281 in cell 371de (its host ff000fe1): relayed to "
                      "the host") == 1);
    REQUIRE(log.Count("E_ENTRY_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kWeb);
  REQUIRE(RelayedTo(p, 0)[0].caster == kGuestActor);
  REQUIRE(RelayedTo(p, 0)[0].entry == std::optional<uint32_t>(1));
  // the guest hears nothing back, and no activation ran: no OpenContainer,
  // nothing else to either game
  REQUIRE(SentTo(p, 1) == 0);
  REQUIRE(SentTo(p, 0) == 1);
  // the leave the same way
  p.Messages().clear();
  {
    LogCapture log;
    Entry(p, 1, 0x14, kWeb, 2);
    REQUIRE(log.Count("[verb:traps] ff000fe2 reports ff000fe2 leaving 37281 "
                      "in cell 371de (its host ff000fe1): relayed to the "
                      "host") == 1);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].entry == std::optional<uint32_t>(2));
  REQUIRE(RelayedTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("A trigger entry from the host, for another's presser, from a "
          "game without the cell, from beyond reach, of an actor or of no "
          "reference is refused by name and sends nothing (D-C2)",
          "[TriggerEntry][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  const auto refused = [&](Networking::UserId user, uint64_t presser,
                           uint64_t trigger, const std::string& code) {
    p.Messages().clear();
    LogCapture log;
    REQUIRE_NOTHROW(Entry(p, user, presser, trigger, 1));
    REQUIRE(log.Count(code) == 1);
    REQUIRE(log.Count("relayed to the host") == 0);
    REQUIRE(RelayedTo(p, 0).empty());
    REQUIRE(RelayedTo(p, 1).empty());
  };
  // the guest's game has not reported the Temple loaded
  refused(1, 0x14, kWeb,
          "[verb:traps] E_ENTRY_NOT_LOADER: ff000fe2 "
          "reports ff000fe2 entering 37281");
  Loaded(p, 1, kTemple);
  // the host's own engine ran the script
  refused(0, 0x14, kWeb,
          "[verb:traps] E_ENTRY_HOST: ff000fe1 reports "
          "ff000fe1 entering 37281");
  // the host's player is the host's own presser, not the guest's
  refused(1, kHostActor, kWeb,
          "[verb:traps] E_ENTRY_PRESSER: ff000fe2 "
          "reports ff000fe1 entering 37281");
  // a caster past 32 bits names no form (from the host's game: a refusal
  // line is one a minute per reporter and code)
  refused(0, 0x1ff000fe2ull, kWeb,
          "[verb:traps] E_ENTRY_PRESSER: ff000fe1 reports 0 entering 37281");
  // an actor is no trigger (judged by its ACHR before any load)
  refused(1, 0x14, kDraugr,
          "[verb:traps] E_ENTRY_ACTOR: ff000fe2 reports "
          "ff000fe2 entering 380b3");
  // an id with no reference and no record
  refused(1, 0x14, 0x12345,
          "[verb:traps] E_ENTRY_NO_REF: ff000fe2 reports "
          "ff000fe2 entering 12345");
  // the guest 2000 units from the web
  Stand(p, kGuestActor, kFarFromTheWeb);
  refused(1, 0x14, kWeb,
          "[verb:traps] E_ENTRY_REACH: ff000fe2 reports "
          "ff000fe2 entering 37281");
  Leave(p);
}

// the train's review, note 2: an entry counts where the engine counts it,
// anywhere inside the trigger's primitive. Bleak Falls Sanctum 0x371dd's
// word wall trigger 0x709d2 (ACTI 0x5095e WordWallTrigger at (-7601, -154,
// 674)) keeps its sphere of radius 2,065 (XPRM type 2, WordWallTest's), but
// its own reach is past it: WordWallTrigger's bounds are the chamber, a box
// of about 4,000 units a side (docs/LOG.md, 2026-11-01's word wall read), so
// the reach (452 plus the bounds' farthest corner times the scale, wire-rules
// activation) holds 1,500 units off (pipeline 5622's red) and the whole
// sphere, and no recorded primitive of the unit world reaches past its own
// reach. So the case gives the held trigger a sphere the engine's own kind
// (type 2) a thousand units past its reach, read from its own bounds: 500
// units past the reach to the south a presser stands inside it, so the
// relay comes from the primitive alone; 500 past the sphere to the north it
// stands outside both, and the origin's reach refuses it
TEST_CASE("A presser inside a trigger's primitive, far from its origin, is "
          "within its reach, and one outside it is not (D-C2)",
          "[TriggerEntry][espm]")
{
  constexpr uint32_t kSanctum = 0x371dd;
  constexpr uint32_t kWallTrigger = 0x709d2;
  const NiPoint3 kAtTheTrigger{ -7601.f, -154.f, 674.f };
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kAtTheTrigger, 0, kSanctum);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kAtTheTrigger, 0, kSanctum);
  p.SetUserActor(1, kGuestActor);
  auto& trigger = p.worldState.GetFormAt<MpObjectReference>(kWallTrigger);
  REQUIRE(trigger.HasPrimitive());
  // the trigger's own reach by its bounds (the handler's
  // ActivationTargetSize: its base's bounds radius times its scale)
  float size = 0.f;
  {
    auto& browser = p.worldState.GetEspm().GetBrowser();
    const auto* base = browser.LookupById(trigger.GetBaseId()).rec;
    const auto bounds = base
      ? espm::GetObjectBounds(base, p.worldState.GetEspmCache())
      : std::nullopt;
    size = bounds ? espm::BoundsRadius(*bounds) *
        espm::GetData<espm::REFR>(kWallTrigger, &p.worldState).scale
                  : 0.f;
  }
  const float reach = skymp::rules::activation_within_reach(0.f, size).bound;
  const float radius = reach + 1000.f;
  trigger.SetPrimitive({ radius, radius, radius }, 2);
  const float inside = reach + 500.f;
  const float outside = radius + 500.f;
  const NiPoint3 kInsideTheSphere{ -7601.f, -154.f - inside, 674.f };
  const NiPoint3 kOutsideTheSphere{ -7601.f, -154.f + outside, 674.f };
  REQUIRE(trigger.HasPrimitive());
  REQUIRE(trigger.IsPointInsidePrimitive(kInsideTheSphere));
  REQUIRE_FALSE(trigger.IsPointInsidePrimitive(kOutsideTheSphere));
  // a sphere is radial, so the case proves the rule only if the presser
  // inside it is past the origin's reach by the trigger's own bounds
  REQUIRE_FALSE(skymp::rules::activation_within_reach(inside, size).allowed);
  REQUIRE_FALSE(skymp::rules::activation_within_reach(outside, size).allowed);
  Stand(p, kHostActor, kAtTheTrigger);
  Stand(p, kGuestActor, kInsideTheSphere);
  Host(p, 0, kSanctum);
  Loaded(p, 1, kSanctum);
  p.Messages().clear();
  {
    LogCapture log;
    Entry(p, 1, 0x14, kWallTrigger, 1);
    REQUIRE(log.Count("[verb:traps] ff000fe2 reports ff000fe2 entering "
                      "709d2 in cell 371dd (its host ff000fe1): relayed to "
                      "the host") == 1);
    REQUIRE(log.Count("E_ENTRY_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kWallTrigger);
  REQUIRE(RelayedTo(p, 0)[0].caster == kGuestActor);
  // outside the sphere the origin's reach is the rule
  Stand(p, kGuestActor, kOutsideTheSphere);
  p.Messages().clear();
  {
    LogCapture log;
    REQUIRE_NOTHROW(Entry(p, 1, 0x14, kWallTrigger, 1));
    REQUIRE(log.Count("[verb:traps] E_ENTRY_REACH: ff000fe2 reports "
                      "ff000fe2 entering 709d2") == 1);
    REQUIRE(log.Count("relayed to the host") == 0);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  Leave(p);
}

// the builder's call on the probe review of D-C2: a guest who leaves a world
// trigger by a teleport or a door reports its OnTriggerLeave after the move,
// far off the trigger. A leave pairs with its presser's accepted enter
// (wire-rules trigger_entry::Entries) and is taken without the reach; a
// leave with none to end, a second one among them, is E_ENTRY_UNPAIRED
TEST_CASE("A guest's leave after its accepted enter is relayed from far off "
          "the trigger, and a leave with no enter to end is refused "
          "E_ENTRY_UNPAIRED (D-C2)",
          "[TriggerEntry][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Loaded(p, 1, kTemple);
  p.Messages().clear();
  // a leave in the web with no enter before it: refused, nothing sent
  {
    LogCapture log;
    REQUIRE_NOTHROW(Entry(p, 1, 0x14, kWeb, 2));
    REQUIRE(log.Count("[verb:traps] E_ENTRY_UNPAIRED: ff000fe2 reports "
                      "ff000fe2 leaving 37281: no accepted entry of ff000fe2 "
                      "into 37281 to end; refused, 1 since the last line") ==
            1);
    REQUIRE(log.Count("relayed to the host") == 0);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  // the enter in the web, relayed
  Entry(p, 1, 0x14, kWeb, 1);
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].entry == std::optional<uint32_t>(1));
  // the guest is moved 2000 units off (a teleport, a door) and reports its
  // leave there: beyond the enter's reach, paired, relayed to the host
  Stand(p, kGuestActor, kFarFromTheWeb);
  p.Messages().clear();
  {
    LogCapture log;
    Entry(p, 1, 0x14, kWeb, 2);
    REQUIRE(log.Count("[verb:traps] ff000fe2 reports ff000fe2 leaving 37281 "
                      "in cell 371de (its host ff000fe1): relayed to the "
                      "host") == 1);
    REQUIRE(log.Count("E_ENTRY_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kWeb);
  REQUIRE(RelayedTo(p, 0)[0].caster == kGuestActor);
  REQUIRE(RelayedTo(p, 0)[0].entry == std::optional<uint32_t>(2));
  REQUIRE(RelayedTo(p, 1).empty());
  // a second leave ends nothing: refused (its line counted, the first said
  // within the minute), nothing sent
  p.Messages().clear();
  {
    LogCapture log;
    REQUIRE_NOTHROW(Entry(p, 1, 0x14, kWeb, 2));
    REQUIRE(log.Count("relayed to the host") == 0);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  // an enter from far off is still the reach's refusal, and opens nothing
  p.Messages().clear();
  {
    LogCapture log;
    REQUIRE_NOTHROW(Entry(p, 1, 0x14, kWeb, 1));
    REQUIRE(log.Count("[verb:traps] E_ENTRY_REACH: ff000fe2 reports "
                      "ff000fe2 entering 37281") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  // so its leave has nothing to end either
  p.Messages().clear();
  REQUIRE_NOTHROW(Entry(p, 1, 0x14, kWeb, 2));
  REQUIRE(RelayedTo(p, 0).empty());
  Leave(p);
}

// the builder's call, the gap closed: a guest who leaves by a door into
// another cell has its game report the trigger's cell unloaded, its actor
// in another world, before the leave arrives. The pairing proves the
// presser was inside (its accepted enter passed the loader and the reach),
// so the leave skips both and still ends the presence on the host
TEST_CASE("A guest's leave after its accepted enter is relayed to the host "
          "after its game unloaded the trigger's cell and its actor left "
          "it (D-C2)",
          "[TriggerEntry][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Loaded(p, 1, kTemple);
  Entry(p, 1, 0x14, kWeb, 1);
  REQUIRE(RelayedTo(p, 0).size() == 1);
  // the door: the guest's game unloads the Temple, its actor out in Tamriel
  {
    RawMessageData raw;
    raw.userId = 1;
    CellLoadedMessage unloaded;
    unloaded.cell = kTemple;
    unloaded.loaded = false;
    p.GetActionListener().OnCellLoaded(raw, unloaded);
  }
  auto& guest = p.worldState.GetFormAt<MpActor>(kGuestActor);
  guest.SetCellOrWorld(FormDesc::Tamriel());
  guest.SetPos({ 4096.f, -4096.f, 0.f });
  // its game is no loader of the Temple any more
  const auto loaders = p.GetCellLoaders(kTemple);
  REQUIRE(std::find(loaders.begin(), loaders.end(), kGuestActor) ==
          loaders.end());
  p.Messages().clear();
  {
    LogCapture log;
    Entry(p, 1, 0x14, kWeb, 2);
    REQUIRE(log.Count("[verb:traps] ff000fe2 reports ff000fe2 leaving 37281 "
                      "in cell 371de (its host ff000fe1): relayed to the "
                      "host") == 1);
    REQUIRE(log.Count("E_ENTRY_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].target == kWeb);
  REQUIRE(RelayedTo(p, 0)[0].caster == kGuestActor);
  REQUIRE(RelayedTo(p, 0)[0].entry == std::optional<uint32_t>(2));
  // an enter keeps the loader: from out here, refused, and its leave has
  // nothing to end
  p.Messages().clear();
  {
    LogCapture log;
    REQUIRE_NOTHROW(Entry(p, 1, 0x14, kWeb, 1));
    REQUIRE(log.Count("[verb:traps] E_ENTRY_NOT_LOADER: ff000fe2 reports "
                      "ff000fe2 entering 37281") == 1);
    REQUIRE_NOTHROW(Entry(p, 1, 0x14, kWeb, 2));
    REQUIRE(log.Count("[verb:traps] E_ENTRY_UNPAIRED: ff000fe2 reports "
                      "ff000fe2 leaving 37281") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  Leave(p);
}

TEST_CASE("An Activate without an entry keeps the activation's own path "
          "(D-C2)",
          "[TriggerEntry][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Loaded(p, 1, kTemple);
  p.Messages().clear();
  // the guest's player activates the web: an activation, judged and played
  // as before (an ACTI answers its activator with OpenContainer), no entry
  // relayed
  {
    LogCapture log;
    ActivateMessage msg;
    msg.data.caster = 0x14;
    msg.data.target = kWeb;
    msg.data.isSecondActivation = false;
    RawMessageData raw;
    raw.userId = 1;
    REQUIRE_NOTHROW(p.GetActionListener().OnActivate(raw, msg));
    REQUIRE(log.Count("relayed to the host") == 0);
    REQUIRE(log.Count("E_ENTRY_") == 0);
  }
  for (const auto& relay : RelayedTo(p, 0)) {
    REQUIRE(!relay.entry);
  }
  bool opened = false;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<OpenContainerMessage*>(m.message.get());
    if (m.userId == 1 && typed && typed->target == kWeb) {
      opened = true;
    }
  }
  REQUIRE(opened);
  Leave(p);
}
