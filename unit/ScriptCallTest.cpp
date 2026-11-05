#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "script_classes/PapyrusObjectReference.h"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>

PartOne& GetPartOne();

// thuum ADR-068 (the one seam): the engine that owns a script's subject
// reports a tabled native the script ran (ScriptCall); the server judges it by
// the table's rule (wire-rules natives) and relays an accepted action at once
// to the subject's other listeners as SpSnippet with the subject's sequence.
// The stage is ObjectAnimationTest's: Bleak Falls Temple 0x371de, the
// portcullis 0x3b9f1 (an ACTI, NorPortcullisLarge01) at (4992, -1408, -768),
// both players beside it; and LockTest's bandit chest 0x371e9 (a CONT,
// TreasBanditChest) at (-3420, 732, 575) in the same temple. The hall door's
// FX (rung 2's merged-head review): Bleak Falls Sanctum 0x371dd's door FX
// 0x5256c, a REFR of RuinsPuzzleDoorFX 0x52172 (an MSTT, which the loader
// never attaches) at (128, -320, -256), which HallofStoriesKeyholeScript's
// openDoor plays FXDoorOpen on (lab/esm.py, 2026-11-05; no override in
// Update.esm or the three DLCs); the temple's draugr 0x380b4, an ACHR the
// fresh world never loads (npcEnabled false; TrapRelayTest's).
namespace {
using Arg = std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>;

constexpr uint32_t kHostActor = 0xff000fe3;
constexpr uint32_t kGuestActor = 0xff000fe4;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPortcullis = 0x3b9f1;
constexpr uint32_t kBanditChest = 0x371e9;
const NiPoint3 kByThePortcullis{ 4992.f, -1508.f, -768.f };
const NiPoint3 kByTheChest{ -3420.f, 732.f, 575.f };
constexpr uint32_t kSanctum = 0x371dd;
constexpr uint32_t kDoorFx = 0x5256c;
constexpr uint32_t kDoorFxBase = 0x52172;
constexpr uint32_t kDraugrUnloaded = 0x380b4;
const NiPoint3 kByTheDoorFx{ 128.f, -220.f, -256.f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// `user`'s game reports `cell` loaded or unloaded (CellLoaded)
void Loaded(PartOne& p, Networking::UserId user, uint32_t cell, bool loaded)
{
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage msg;
  msg.cell = cell;
  msg.loaded = loaded;
  p.GetActionListener().OnCellLoaded(raw, msg);
}

// `user` hosts `cell` (TrapRelayTest's shape)
void Host(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  Loaded(p, user, cell, true);
  p.TickCellHosts(past);
}

// the host (user 0) and the guest (user 1) at `where` in the temple, the host
// hosting it; returns the grant's epoch
uint32_t Enter(PartOne& p, const NiPoint3& where = kByThePortcullis)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, where, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, where, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  // the handler looks a subject up without a load: the references stand
  // loaded, as a hosted cell's do
  p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  p.worldState.GetFormAt<MpObjectReference>(kBanditChest);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(where);
    actor.ForceSubscriptionsUpdate();
  }
  Host(p, 0, kTemple);
  REQUIRE(p.GetCellHost(kTemple).hosted);
  REQUIRE(p.GetCellHost(kTemple).player == kHostActor);
  p.Messages().clear();
  return p.GetCellHost(kTemple).epoch;
}

// the host (user 0) and the guest (user 1) by the door FX in the Sanctum, the
// host hosting it and the guest's game reporting it loaded; returns the
// grant's epoch
uint32_t EnterSanctum(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kByTheDoorFx, 0, kSanctum);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kByTheDoorFx, 0, kSanctum);
  p.SetUserActor(1, kGuestActor);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(kByTheDoorFx);
    actor.ForceSubscriptionsUpdate();
  }
  Host(p, 0, kSanctum);
  Loaded(p, 1, kSanctum, true);
  // the second loader takes nothing from the host
  REQUIRE(p.GetCellHost(kSanctum).hosted);
  REQUIRE(p.GetCellHost(kSanctum).player == kHostActor);
  const std::vector<uint32_t> bothGames{ kHostActor, kGuestActor };
  REQUIRE(p.GetCellLoaders(kSanctum) == bothGames);
  p.Messages().clear();
  return p.GetCellHost(kSanctum).epoch;
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

void Call(PartOne& p, Networking::UserId user, uint32_t epoch, uint32_t selfId,
          const char* cls, const char* fn, std::vector<Arg> args,
          const char* caller = "default2StateActivator",
          const char* handler = "onActivate")
{
  ScriptCallMessage msg;
  msg.epoch = epoch;
  msg.selfId = selfId;
  msg.cls = cls;
  msg.fn = fn;
  msg.args = std::move(args);
  msg.caller = caller;
  msg.handler = handler;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
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
    auto logger = std::make_shared<spdlog::logger>("script-call-test", sink);
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

Arg Text(const char* s)
{
  return Arg(std::string(s));
}

Arg FormArg(uint32_t formId, const char* type)
{
  SpSnippetObjectArgument object;
  object.formId = formId;
  object.type = type;
  return Arg(object);
}

// the snippets sent to `user`, as the fake send target recorded them (JSON)
std::vector<nlohmann::json> SnippetsTo(PartOne& p, Networking::UserId user)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j["t"] == static_cast<int>(MsgType::SpSnippet)) {
      out.push_back(m.j);
    }
  }
  return out;
}

std::optional<std::string> LastAnimation(PartOne& p, uint32_t refr)
{
  return p.worldState.GetFormAt<MpObjectReference>(refr)
    .GetChangeForm()
    .lastAnimation;
}

// HallofStoriesKeyholeScript.openDoor's first native: PlayAnimation of
// FXDoorOpen on the door FX (or on `selfId`), reported by `user` at `epoch`
void OpenDoorFx(PartOne& p, Networking::UserId user, uint32_t epoch,
                uint32_t selfId = kDoorFx)
{
  Call(p, user, epoch, selfId, "ObjectReference", "PlayAnimation",
       { Text("FXDoorOpen") }, "HallofStoriesKeyholeScript", "openDoor");
}
}

TEST_CASE("The host's script call is relayed at once to the cell's other "
          "game with the subject's sequence",
          "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  Call(p, 0, epoch, kPortcullis, "ObjectReference", "PlayAnimation",
       { Text("Open") });
  // no tick: the relay never waits in the deferred queue (6.11 item 1)
  auto relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["class"] == "ObjectReference");
  REQUIRE(relayed[0]["function"] == "PlayAnimation");
  REQUIRE(relayed[0]["selfId"] == kPortcullis);
  REQUIRE(relayed[0]["arguments"] == nlohmann::json::array({ "Open" }));
  REQUIRE(relayed[0]["snippetIdx"] == std::numeric_limits<uint32_t>::max());
  REQUIRE(relayed[0]["seq"] == 1);
  // never back to the reporter
  REQUIRE(SnippetsTo(p, 0).empty());
  // the PlayAnimation pair keeps its event as the reference's lastAnimation
  REQUIRE(LastAnimation(p, kPortcullis) == std::optional<std::string>("Open"));

  // the next call on the same object takes the next sequence
  p.Messages().clear();
  Call(p, 0, epoch, kPortcullis, "ObjectReference", "PlayAnimation",
       { Text("Close") });
  relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["seq"] == 2);
  REQUIRE(relayed[0]["arguments"] == nlohmann::json::array({ "Close" }));
  REQUIRE(LastAnimation(p, kPortcullis) ==
          std::optional<std::string>("Close"));
  Leave(p);
}

TEST_CASE("PlayAnimationAndWait is relayed as its twin PlayAnimation",
          "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  Call(p, 0, epoch, kPortcullis, "ObjectReference", "PlayAnimationAndWait",
       { Text("Trigger02"), Text("Turned02") });
  const auto relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["class"] == "ObjectReference");
  REQUIRE(relayed[0]["function"] == "PlayAnimation");
  REQUIRE(relayed[0]["selfId"] == kPortcullis);
  REQUIRE(relayed[0]["arguments"] == nlohmann::json::array({ "Trigger02" }));
  REQUIRE(relayed[0]["seq"] == 1);
  REQUIRE(LastAnimation(p, kPortcullis) ==
          std::optional<std::string>("Trigger02"));
  Leave(p);
}

TEST_CASE("A static Debug.SendAnimationEvent is relayed with no self, its "
          "target first, with the target's sequence",
          "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  // the call's selfId is its subject, the target, named again as the first
  // argument
  Call(p, 0, epoch, kPortcullis, "Debug", "SendAnimationEvent",
       { FormArg(kPortcullis, "ObjectReference"), Text("Open") });
  auto relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["class"] == "Debug");
  REQUIRE(relayed[0]["function"] == "SendAnimationEvent");
  REQUIRE(relayed[0]["selfId"] == 0);
  REQUIRE(relayed[0]["arguments"].size() == 2);
  REQUIRE(relayed[0]["arguments"][0]["formId"] == kPortcullis);
  REQUIRE(relayed[0]["arguments"][0]["type"] == "ObjectReference");
  REQUIRE(relayed[0]["arguments"][1] == "Open");
  REQUIRE(relayed[0]["seq"] == 1);
  REQUIRE(SnippetsTo(p, 0).empty());
  // the row keeps nothing
  REQUIRE(!LastAnimation(p, kPortcullis).has_value());

  // one sequence per object, whichever row moved it
  p.Messages().clear();
  Call(p, 0, epoch, kPortcullis, "ObjectReference", "PlayAnimation",
       { Text("Close") });
  relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["seq"] == 2);
  Leave(p);
}

TEST_CASE("A guest's script call, an untabled pair, a wrong argument, a stale "
          "epoch and an actor's call are refused and relayed to nobody",
          "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  // E_SCRIPTCALL_OWNER: the guest does not host the temple
  Call(p, 1, epoch, kPortcullis, "ObjectReference", "PlayAnimation",
       { Text("Open") });
  // E_SCRIPTCALL_OWNER: the host's report names an epoch the cell is not at
  Call(p, 0, epoch + 1, kPortcullis, "ObjectReference", "PlayAnimation",
       { Text("Open") });
  // E_SCRIPTCALL_ROW: a pair the table lacks, and a state row of the flip
  Call(p, 0, epoch, kPortcullis, "Debug", "Notification", { Text("hi") });
  Call(p, 0, epoch, kPortcullis, "ObjectReference", "Enable", { Arg(false) });
  // E_SCRIPTCALL_ARGS: the row's count and kinds, a static call's selfId
  // other than its target
  Call(p, 0, epoch, kPortcullis, "ObjectReference", "PlayAnimation", {});
  Call(p, 0, epoch, kPortcullis, "ObjectReference", "PlayAnimation",
       { Arg(1.0) });
  Call(p, 0, epoch, 0, "Debug", "SendAnimationEvent",
       { FormArg(kPortcullis, "ObjectReference"), Text("Open") });
  // E_SCRIPTCALL_ROW: an actor's animation is the streams', the reporter's
  // own player (0x14) among them; E_SCRIPTCALL_ARGS: a figure the server
  // names
  Call(p, 0, epoch, 0x14, "ObjectReference", "PlayAnimation",
       { Text("Open") });
  Call(p, 0, epoch, kGuestActor, "ObjectReference", "PlayAnimation",
       { Text("Open") });
  REQUIRE(SnippetsTo(p, 0).empty());
  REQUIRE(SnippetsTo(p, 1).empty());
  REQUIRE(!LastAnimation(p, kPortcullis).has_value());

  // and no refusal took a sequence: the host's own call is the first
  Call(p, 0, epoch, kPortcullis, "ObjectReference", "PlayAnimation",
       { Text("Open") });
  const auto relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["seq"] == 1);
  Leave(p);
}

TEST_CASE("A container's script call is dropped quietly", "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  // both players beside the chest, so a relay would reach the guest
  const uint32_t epoch = Enter(p, kByTheChest);
  Call(p, 0, epoch, kBanditChest, "ObjectReference", "PlayAnimation",
       { Text("Open") });
  // its lid is isOpen's (Activate's path), as OnObjectAnimation drops it
  REQUIRE(SnippetsTo(p, 0).empty());
  REQUIRE(SnippetsTo(p, 1).empty());
  REQUIRE(!LastAnimation(p, kBanditChest).has_value());
  Leave(p);
}

TEST_CASE("A reporter's script calls past its budget are refused and relayed "
          "to nobody",
          "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  // the reporter's budget (wire-rules natives: BURST 32 at once, then
  // PER_SECOND 8 a second) runs on the server's own monotonic clock, so the
  // count allows the refill the loop's own time bought (a fixed count would
  // flake on a slow runner)
  const uint64_t startMs = NowMs();
  size_t sent = 0;
  size_t relayed = 0;
  bool refused = false;
  while (!refused && sent < 200) {
    Call(p, 0, epoch, kPortcullis, "ObjectReference", "PlayAnimation",
         { Text("Open") });
    ++sent;
    const size_t now = SnippetsTo(p, 1).size();
    refused = now == relayed;
    relayed = now;
  }
  const uint64_t elapsedMs = NowMs() - startMs;
  REQUIRE(refused);
  REQUIRE(relayed >= 32);
  REQUIRE(relayed <= 32 + elapsedMs * 8 / 1000 + 1);
  // the refused call took no sequence: the last relay's is the count
  REQUIRE(SnippetsTo(p, 1).back()["seq"] == relayed);
  Leave(p);
}

TEST_CASE("The server VM's own snippet carries no sequence and waits for "
          "the tick",
          "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  PapyrusObjectReference papyrusObjectReference;
  auto& portcullis = p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  papyrusObjectReference.PlayAnimation(portcullis.ToVarValue(),
                                       { VarValue(std::string("Open")) });
  // the deferred queue: nothing before the tick
  REQUIRE(SnippetsTo(p, 1).empty());
  p.Tick();
  const auto snippets = SnippetsTo(p, 1);
  REQUIRE(snippets.size() == 1);
  REQUIRE(snippets[0]["function"] == "PlayAnimation");
  REQUIRE(snippets[0]["selfId"] == kPortcullis);
  REQUIRE(!snippets[0].contains("seq"));
  Leave(p);
}

TEST_CASE("The host's script call on a placement the server never loads is "
          "judged by its record and mirrored to the other games with its "
          "cell loaded",
          "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = EnterSanctum(p);
  // the lines the held path writes, byte for byte (x-bleak-scripts' and
  // x-puzzle-whale's patterns, b-puzzle-relay's animates line)
  const std::string animates = "[verb:traps] 5256c animates 5256c "
                               "'FXDoorOpen' in cell 371dd (its host "
                               "ff000fe3 reports): recorded, relayed to ";
  const std::string reports = "[verb:scripts] ff000fe3 reports "
                              "ObjectReference.PlayAnimation on 5256c in "
                              "cell 371dd (epoch " +
    std::to_string(epoch) +
    ", from HallofStoriesKeyholeScript.openDoor): seq ";
  {
    LogCapture log;
    OpenDoorFx(p, 0, epoch);
    REQUIRE(log.Count(animates + "1 game(s)") == 1);
    REQUIRE(log.Count(reports +
                      "1, relayed to 1 game(s) as "
                      "ObjectReference.PlayAnimation") == 1);
    REQUIRE(log.Count("E_SCRIPTCALL_") == 0);
  }
  // judged by its record: the server made no reference of it
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kDoorFx) == nullptr);
  auto relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["class"] == "ObjectReference");
  REQUIRE(relayed[0]["function"] == "PlayAnimation");
  REQUIRE(relayed[0]["selfId"] == kDoorFx);
  REQUIRE(relayed[0]["arguments"] == nlohmann::json::array({ "FXDoorOpen" }));
  REQUIRE(relayed[0]["snippetIdx"] == std::numeric_limits<uint32_t>::max());
  REQUIRE(relayed[0]["seq"] == 1);
  // never back to the reporter
  REQUIRE(SnippetsTo(p, 0).empty());

  // the next call on the object takes its next sequence
  p.Messages().clear();
  OpenDoorFx(p, 0, epoch);
  relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["seq"] == 2);

  // a game that unloads the cell hears the object no more
  Loaded(p, 1, kSanctum, false);
  REQUIRE(p.GetCellLoaders(kSanctum) == std::vector<uint32_t>(1, kHostActor));
  p.Messages().clear();
  {
    LogCapture log;
    OpenDoorFx(p, 0, epoch);
    REQUIRE(log.Count(animates + "0 game(s)") == 1);
    REQUIRE(log.Count(reports +
                      "3, relayed to 0 game(s) as "
                      "ObjectReference.PlayAnimation") == 1);
  }
  REQUIRE(SnippetsTo(p, 0).empty());
  REQUIRE(SnippetsTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("A script call on a placement by its record from a game that does "
          "not host its cell at the epoch is refused and mirrored to nobody",
          "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = EnterSanctum(p);
  const std::string byRecord = " (a reference the server does not load, "
                               "placed by its game-file record)";
  {
    LogCapture log;
    // E_SCRIPTCALL_OWNER: the guest has the Sanctum loaded but does not
    // host it
    OpenDoorFx(p, 1, epoch);
    // E_SCRIPTCALL_OWNER: the host's report names an epoch the cell is not
    // at
    OpenDoorFx(p, 0, epoch + 1);
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_OWNER: ff000fe4 reports "
                      "ObjectReference.PlayAnimation on 5256c (epoch " +
                      std::to_string(epoch) +
                      ", from HallofStoriesKeyholeScript.openDoor): "
                      "ff000fe3 hosts cell 371dd, not the reporter" +
                      byRecord + "; refused, 1 since the last line") == 1);
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_OWNER: ff000fe3 reports "
                      "ObjectReference.PlayAnimation on 5256c (epoch " +
                      std::to_string(epoch + 1) +
                      ", from HallofStoriesKeyholeScript.openDoor): cell "
                      "371dd stands at epoch " +
                      std::to_string(epoch) + ", the report names " +
                      std::to_string(epoch + 1) + byRecord +
                      "; refused, 1 since the last line") == 1);
    REQUIRE(log.Count(" animates ") == 0);
  }
  REQUIRE(SnippetsTo(p, 0).empty());
  REQUIRE(SnippetsTo(p, 1).empty());

  // and no refusal took a sequence: the host's own call is the first
  OpenDoorFx(p, 0, epoch);
  const auto relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["seq"] == 1);
  Leave(p);
}

TEST_CASE("A script call on an actor's placement or on a record that is no "
          "placement keeps the refusal of a subject the server does not hold",
          "[ScriptCall][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = EnterSanctum(p);
  {
    LogCapture log;
    // the temple's draugr, an ACHR (an actor's placement: the streams'),
    // from the host; the door FX's base, an MSTT record and no placement,
    // from the guest (a refusal line is once a minute per reporter and
    // code, so one reporter each)
    OpenDoorFx(p, 0, epoch, kDraugrUnloaded);
    OpenDoorFx(p, 1, epoch, kDoorFxBase);
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_OWNER: ff000fe3 reports "
                      "ObjectReference.PlayAnimation on 380b4 (epoch " +
                      std::to_string(epoch) +
                      ", from HallofStoriesKeyholeScript.openDoor): the "
                      "server holds no such reference; refused, 1 since the "
                      "last line") == 1);
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_OWNER: ff000fe4 reports "
                      "ObjectReference.PlayAnimation on 52172 (epoch " +
                      std::to_string(epoch) +
                      ", from HallofStoriesKeyholeScript.openDoor): the "
                      "server holds no such reference; refused, 1 since the "
                      "last line") == 1);
    REQUIRE(log.Count("placed by its game-file record") == 0);
  }
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kDraugrUnloaded) == nullptr);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kDoorFxBase) == nullptr);
  REQUIRE(SnippetsTo(p, 0).empty());
  REQUIRE(SnippetsTo(p, 1).empty());
  Leave(p);
}
