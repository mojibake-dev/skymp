#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <array>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>

PartOne& GetPartOne();

// thuum docs/DECISIONS.md ADR-068 D-W2 A (Eli 2026-11-07; docs/research/
// fifty-fixes-2026-11-07.md section 7.2(b)): during a firing its presser's
// owner started, that seat owns the trap family trigger's own calls on
// itself, which cross from the firing seat as ScriptCall and play on every
// other seat: the look channel (wire-rules natives), judged by the firing's
// own rule over the subject's gathered facts (the cell's host at the epoch,
// or a game with the cell loaded one of whose actors stands within the
// subject's firing reach), relayed and kept nowhere. The stage is
// TrapRelayTest's: Bleak Falls Temple 0x371de and its plate 0x567f2 at
// (8589, -5338, -2054), whose PressurePlate plays TriggerSound.Play(self)
// and then 'Down'. The plate stands in for the other triggers' calls too
// (the server never reads which script made a call): the Tripwire's
// SetMotionType on its rope and its sound TRPTripwire 0x1d492, and the bone
// alarm's CreateDetectionEvent (docs/research/trap-census-2026-11-05.md).
// Three seats: the host, the guest that pressed the plate, and a second
// guest beside it, so a relay to the host alone shows.
namespace {
using Arg = std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>;

constexpr uint32_t kHostActor = 0xff000ff5;
constexpr uint32_t kGuestActor = 0xff000ff6;
constexpr uint32_t kWatcherActor = 0xff000ff7;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPlate = 0x567f2;
constexpr uint32_t kTripwireSound = 0x1d492;
const NiPoint3 kByThePlate{ 8589.f, -5438.f, -2054.f };
const NiPoint3 kByThePortcullis{ 4992.f, -1508.f, -768.f };
// the host's game and the watcher's: the games a guest's look reaches
const std::array<Networking::UserId, 2> kOthers{ 0, 2 };

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

// `user` hosts `cell` (TrapRelayTest's shape)
void Host(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  Loaded(p, user, cell);
  p.TickCellHosts(past);
}

void Place(PartOne& p, uint32_t id, const NiPoint3& pos)
{
  auto& actor = p.worldState.GetFormAt<MpActor>(id);
  actor.SetPos(pos);
  actor.ForceSubscriptionsUpdate();
}

// the three seats by the plate, the host's game hosting the temple and the
// guest's with it loaded; the watcher's game has not reported it; returns
// the grant's epoch
uint32_t Enter(PartOne& p)
{
  const uint32_t ids[] = { kHostActor, kGuestActor, kWatcherActor };
  for (Networking::UserId user = 0; user < 3; ++user) {
    DoConnect(p, user);
    p.CreateActor(ids[user], kByThePlate, 0, kTemple);
    p.SetUserActor(user, ids[user]);
  }
  p.worldState.GetFormAt<MpObjectReference>(kPlate);
  for (uint32_t id : ids) {
    Place(p, id, kByThePlate);
  }
  Host(p, 0, kTemple);
  Loaded(p, 1, kTemple);
  REQUIRE(p.GetCellHost(kTemple).player == kHostActor);
  p.Messages().clear();
  return p.GetCellHost(kTemple).epoch;
}

void Leave(PartOne& p)
{
  for (Networking::UserId user = 0; user < 3; ++user) {
    DoDisconnect(p, user);
  }
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
  p.DestroyActor(kWatcherActor);
}

// a trigger's own call as `user`'s engine reports it (kind "look" crosses
// as any ScriptCall); a guest holds no grant and names epoch 0
void Call(PartOne& p, Networking::UserId user, uint32_t epoch, uint32_t selfId,
          const char* cls, const char* fn, std::vector<Arg> args)
{
  ScriptCallMessage msg;
  msg.epoch = epoch;
  msg.selfId = selfId;
  msg.cls = cls;
  msg.fn = fn;
  msg.args = std::move(args);
  msg.caller = "PressurePlate";
  msg.handler = "onBeginState";
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
}

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
      std::make_shared<spdlog::logger>("script-call-look-test", sink);
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

TEST_CASE("A guest's look by the plate is relayed to the cell's other games "
          "with the plate's sequence and kept nowhere (D-W2)",
          "[ScriptCall][Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  auto& plate = p.worldState.GetFormAt<MpObjectReference>(kPlate);
  {
    LogCapture log;
    Call(p, 1, 0, kPlate, "ObjectReference", "PlayAnimation",
         { Text("Down") });
    REQUIRE(log.Count("[verb:traps] ff000ff6 reports "
                      "ObjectReference.PlayAnimation on 567f2 in cell 371de "
                      "(epoch 0, from PressurePlate.onBeginState): its firing "
                      "seat's look, seq 1, relayed to 2 game(s) as "
                      "ObjectReference.PlayAnimation, kept nowhere (D-W2)") ==
            1);
    REQUIRE(log.Count("E_SCRIPTCALL_") == 0);
    // a look is no recorded animation: neither line of the host's own
    REQUIRE(log.Count(" animates ") == 0);
  }
  for (const Networking::UserId user : kOthers) {
    const auto relayed = SnippetsTo(p, user);
    REQUIRE(relayed.size() == 1);
    REQUIRE(relayed[0]["class"] == "ObjectReference");
    REQUIRE(relayed[0]["function"] == "PlayAnimation");
    REQUIRE(relayed[0]["selfId"] == kPlate);
    REQUIRE(relayed[0]["arguments"] == nlohmann::json::array({ "Down" }));
    REQUIRE(relayed[0]["seq"] == 1);
  }
  REQUIRE(SnippetsTo(p, 1).empty());
  // kept nowhere: a late joiner never replays a firing's look
  REQUIRE(!plate.GetLastAnimation().has_value());

  // the plate's sound: TriggerSound.Play(self), its self the Sound, relayed
  // on the Sound with no sequence (the Sound is shared by many triggers)
  p.Messages().clear();
  Call(p, 1, 0, kTripwireSound, "Sound", "Play",
       { FormArg(kPlate, "ObjectReference") });
  for (const Networking::UserId user : kOthers) {
    const auto relayed = SnippetsTo(p, user);
    REQUIRE(relayed.size() == 1);
    REQUIRE(relayed[0]["class"] == "Sound");
    REQUIRE(relayed[0]["function"] == "Play");
    REQUIRE(relayed[0]["selfId"] == kTripwireSound);
    REQUIRE(relayed[0]["arguments"][0]["formId"] == kPlate);
    REQUIRE(!relayed[0].contains("seq"));
  }

  // a look-only motion row crosses as itself, with the plate's next
  // sequence (the sound took none)
  p.Messages().clear();
  Call(p, 1, 0, kPlate, "ObjectReference", "SetMotionType",
       { Arg(1.0), Arg(true) });
  const auto motion = SnippetsTo(p, 0);
  REQUIRE(motion.size() == 1);
  REQUIRE(motion[0]["function"] == "SetMotionType");
  REQUIRE(motion[0]["arguments"] == nlohmann::json::array({ 1.0, true }));
  REQUIRE(motion[0]["seq"] == 2);
  Leave(p);
}

TEST_CASE("A guest's look CreateDetectionEvent reaches the cell's host "
          "alone, its own player named as its figure (D-W2)",
          "[ScriptCall][Traps][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  {
    LogCapture log;
    Call(p, 1, 0, kPlate, "ObjectReference", "CreateDetectionEvent",
         { FormArg(0x14, "Actor"), Arg(100.0) });
    REQUIRE(log.Count("[verb:traps] ff000ff6 reports "
                      "ObjectReference.CreateDetectionEvent on 567f2 in cell "
                      "371de (epoch 0, from PressurePlate.onBeginState): its "
                      "firing seat's look, seq 1, relayed to 1 game(s) (the "
                      "cell's host alone) as "
                      "ObjectReference.CreateDetectionEvent, kept nowhere "
                      "(D-W2)") == 1);
  }
  const auto relayed = SnippetsTo(p, 0);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["class"] == "ObjectReference");
  REQUIRE(relayed[0]["function"] == "CreateDetectionEvent");
  REQUIRE(relayed[0]["selfId"] == kPlate);
  // the guest's own player is its figure on the host: the guest's actor
  REQUIRE(relayed[0]["arguments"][0]["formId"] == kGuestActor);
  REQUIRE(relayed[0]["arguments"][1] == 100.0);
  // the watcher beside the plate hears nothing of it, nor the guest
  REQUIRE(SnippetsTo(p, 2).empty());
  REQUIRE(SnippetsTo(p, 1).empty());

  // the host's own detection event: its engine ran it, so nobody else hears
  // it (the host alone, never back to the reporter)
  p.Messages().clear();
  const uint32_t epoch = p.GetCellHost(kTemple).epoch;
  Call(p, 0, epoch, kPlate, "ObjectReference", "CreateDetectionEvent",
       { FormArg(0x14, "Actor"), Arg(100.0) });
  REQUIRE(SnippetsTo(p, 0).empty());
  REQUIRE(SnippetsTo(p, 1).empty());
  REQUIRE(SnippetsTo(p, 2).empty());
  Leave(p);
}

TEST_CASE("A look from a game far from the plate is E_SCRIPTCALL_FAR, from "
          "one without the cell E_SCRIPTCALL_OWNER, and the host under "
          "another grant keeps its own refusal (D-W2)",
          "[ScriptCall][Traps][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  auto& plate = p.worldState.GetFormAt<MpObjectReference>(kPlate);
  {
    LogCapture log;
    // the guest across the temple
    Place(p, kGuestActor, kByThePortcullis);
    Call(p, 1, 0, kPlate, "ObjectReference", "PlayAnimation",
         { Text("Down") });
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_FAR: ff000ff6 reports "
                      "ObjectReference.PlayAnimation on 567f2 (epoch 0, from "
                      "PressurePlate.onBeginState): a game with cell 371de "
                      "loaded whose nearest actor stands ") == 1);
    // the watcher's game never reported the temple loaded
    Call(p, 2, 0, kPlate, "ObjectReference", "SetMotionType",
         { Arg(1.0), Arg(true) });
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_OWNER: ff000ff7 reports "
                      "ObjectReference.SetMotionType on 567f2 (epoch 0, from "
                      "PressurePlate.onBeginState): ff000ff5 hosts cell "
                      "371de, not the reporter; refused, 1 since the last "
                      "line") == 1);
    // the host's report under another grant: the own channel's refusal,
    // never a look, though its player stands by the plate
    Call(p, 0, epoch + 1, kPlate, "ObjectReference", "PlayAnimation",
         { Text("Down") });
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_OWNER: ff000ff5 reports "
                      "ObjectReference.PlayAnimation on 567f2 (epoch " +
                      std::to_string(epoch + 1) +
                      ", from PressurePlate.onBeginState): cell 371de stands "
                      "at epoch " +
                      std::to_string(epoch) + ", the report names " +
                      std::to_string(epoch + 1) +
                      "; refused, 1 since the last line") == 1);
    REQUIRE(log.Count("its firing seat's look") == 0);
  }
  REQUIRE(SnippetsTo(p, 0).empty());
  REQUIRE(SnippetsTo(p, 1).empty());
  REQUIRE(SnippetsTo(p, 2).empty());

  // the host's own action at the epoch keeps its channel: recorded as the
  // plate's lastAnimation, the first sequence (no refusal took one)
  {
    LogCapture log;
    Call(p, 0, epoch, kPlate, "ObjectReference", "PlayAnimation",
         { Text("Down") });
    REQUIRE(log.Count("[verb:traps] 567f2 animates 567f2 'Down' in cell "
                      "371de (its host ff000ff5 reports): recorded, relayed "
                      "to 2 game(s)") == 1);
    REQUIRE(log.Count("its firing seat's look") == 0);
  }
  REQUIRE(plate.GetLastAnimation() == std::optional<std::string>("Down"));
  const auto relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["seq"] == 1);
  Leave(p);
}
