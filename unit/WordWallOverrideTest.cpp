#include "ActionListener.h"
#include "Messages.h"
#include "SpSnippet.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <fmt/format.h>
#include <string>
#include <vector>

PartOne& GetPartOne();

// A word wall teaches each player once, and its world effects happen once
// (docs/verbs/word-wall.md's rule, a script variable the player's own record
// decides). WordWallTriggerScript's StartFX
// goes to its state "done" and sets WordLearned in the block that teaches,
// and WordWallTriggerBleakFallsScript's enter goes to "done" when WordLearned
// is set (the scripts' pex, Skyrim - Misc.bsa, read with lab/pex.py), so the
// trigger's scripts, which its cell's host runs, never play the wall again for
// a second player. A player who lacks the word and reports a wall whose record
// shows the wall's script in "done" gets that state put back on the owner's
// copy, by name (ScriptStateOverride, MsgType 89), then its walk-up raised
// there as a relayed entry (RelayedActivation's entry 1), once a player and
// wall. Bleak Falls Sanctum 0x371dd, its trigger 0x709d2 and the word WordFus
// 0x13e22 (WordWallTest.cpp's reads).
namespace {
constexpr uint32_t kHostActor = 0xff000f7a;
constexpr uint32_t kGuestActor = 0xff000f7b;
constexpr uint32_t kSanctum = 0x371dd;
constexpr uint32_t kTrigger = 0x709d2;
constexpr uint32_t kWord = 0x13e22;
// the override's MsgType (the wire's 97)
constexpr int kScriptStateOverride = 89;
// the trigger's sphere, about 1,860 units off the word's mark
const NiPoint3 kFarInTheSphere{ -7601.f, -1654.f, 674.f };
// about 100 units off the mark
const NiPoint3 kByTheWord{ -7800.f, 150.f, 600.f };

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
  CellLoadedMessage msg;
  msg.cell = cell;
  msg.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, msg);
}

// the host (user 0) and the guest (user 1) in the sanctum, the host hosting it
void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kFarInTheSphere, 0, kSanctum);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kFarInTheSphere, 0, kSanctum);
  p.SetUserActor(1, kGuestActor);
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  Loaded(p, 0, kSanctum);
  p.TickCellHosts(past);
  Loaded(p, 1, kSanctum);
  REQUIRE(p.GetCellHost(kSanctum).hosted);
  REQUIRE(p.GetCellHost(kSanctum).player == kHostActor);
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  auto& trigger = p.worldState.GetFormAt<MpObjectReference>(kTrigger);
  trigger.SetScriptState(std::nullopt);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

// the trigger's record as its owner reports it, its two scripts in `state`
void RecordWall(PartOne& p, const char* state)
{
  ScriptStateRecord record;
  record.epoch = 1;
  record.sender = kHostActor;
  for (const char* type :
       { "WordWallTriggerScript", "WordWallTriggerBleakFallsScript" }) {
    ScriptStateObject script;
    script.type = type;
    script.state = state;
    script.flags = 0x0b;
    record.scripts.push_back(script);
  }
  p.worldState.GetFormAt<MpObjectReference>(kTrigger).SetScriptState(record);
}

void ReportWall(PartOne& p, Networking::UserId user)
{
  DoMessage(p, user,
            nlohmann::json{ { "t", static_cast<int>(MsgType::WordWall) },
                            { "wall", kTrigger } });
}

// the overrides of the trigger and the relayed entries into it sent to
// `user`, in order: "override" or "enter <caster>"
std::vector<std::string> SentAboutTheWall(PartOne& p, Networking::UserId user)
{
  p.Tick();
  std::vector<std::string> out;
  for (auto& m : p.Messages()) {
    if (m.userId != user) {
      continue;
    }
    if (m.j["t"] == kScriptStateOverride && m.j["refrId"] == kTrigger) {
      out.push_back("override");
    } else if (m.j["t"] == static_cast<int>(MsgType::RelayedActivation) &&
               m.j["target"] == kTrigger && m.j["entry"] == 1) {
      out.push_back(fmt::format("enter {:x}", m.j["caster"].get<uint32_t>()));
    }
  }
  return out;
}
}

TEST_CASE("A guest who lacks a word wall's word gets nothing while the wall "
          "has not fired, then, once it has, the wall's player-decided state "
          "put back on its owner's copy by name and its walk-up raised, once",
          "[WordWallOverride][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  // the wall's scripts still listening, or no record: the engine's own first
  // run plays the wall, nothing is put back
  RecordWall(p, "Listening");
  ReportWall(p, 1);
  REQUIRE(SentAboutTheWall(p, 0).empty());
  p.worldState.GetFormAt<MpObjectReference>(kTrigger).SetScriptState(
    std::nullopt);
  ReportWall(p, 1);
  REQUIRE(SentAboutTheWall(p, 0).empty());
  // the wall fired for someone: put back, then the guest's walk-up raised
  RecordWall(p, "done");
  ReportWall(p, 1);
  REQUIRE(SentAboutTheWall(p, 0) ==
          std::vector<std::string>{ "override", "enter ff000f7b" });
  REQUIRE(SentAboutTheWall(p, 1).empty());
  // the override names each script's state to go back to and the wall's
  // learned flag, as the owner's game resolves them by name
  nlohmann::json override;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == kScriptStateOverride) {
      override = m.j;
    }
  }
  REQUIRE(override["scripts"].size() == 2);
  REQUIRE(override["scripts"][0]["type"] == "WordWallTriggerScript");
  REQUIRE(override["scripts"][0]["state"] == "");
  REQUIRE(override["scripts"][0]["vars"].size() == 1);
  REQUIRE(override["scripts"][0]["vars"][0]["name"] == "::wordLearned_var");
  REQUIRE(override["scripts"][0]["vars"][0]["k"] == 5);
  REQUIRE(override["scripts"][0]["vars"][0]["b"] == false);
  REQUIRE(override["scripts"][1]["type"] == "WordWallTriggerBleakFallsScript");
  REQUIRE(override["scripts"][1]["state"] == "Listening");
  REQUIRE(override["scripts"][1]["vars"].empty());
  // the guest's next report and its step to the word (the armed report's
  // replay, which teaches) send nothing more to the owner
  p.Messages().clear();
  ReportWall(p, 1);
  auto& guest = p.worldState.GetFormAt<MpActor>(kGuestActor);
  guest.SetPos(kByTheWord);
  ReportWall(p, 1);
  REQUIRE(guest.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(SentAboutTheWall(p, 0).empty());
  Leave(p);
}

TEST_CASE("At a fired word wall a guest who knows the word gets nothing, and "
          "the host of the wall's cell who lacks it gets the wall put back on "
          "its own copy and its own walk-up raised",
          "[WordWallOverride][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  RecordWall(p, "done");
  auto& guest = p.worldState.GetFormAt<MpActor>(kGuestActor);
  guest.RecordShoutEntry(MpActor::ShoutEntry::WordKnown,
                         FormDesc::FromFormId(kWord, p.worldState.espmFiles));
  ReportWall(p, 1);
  REQUIRE(SentAboutTheWall(p, 0).empty());
  ReportWall(p, 0);
  REQUIRE(SentAboutTheWall(p, 0) ==
          std::vector<std::string>{ "override", "enter ff000f7a" });
  Leave(p);
}

// A fired wall's trigger on and off is each player's: the wall's own Disable
// of its trigger (WordWallTriggerBleakFallsScript's "done" state, from its
// onBeginState) reaches as disabled only the games whose players know the
// wall's word, the owner's copy is turned back on while its own player lacks
// it, a later player is served the trigger by its own record, and a repeat
// of the Disable still turns off the copy of a player who has learned since.
// A disabled trigger raises no entry for anyone (the engine's trigger pass
// skips a form with its disabled flag set), so one value for the world would
// leave every later learner without its walk-up.
namespace {
using Arg = std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>;
constexpr uint32_t kLateActor = 0xff000f7c;

// the owner's Disable of the trigger, as its game's script channel reports
// the wall's own call
void WallDisables(PartOne& p, Networking::UserId user)
{
  ScriptCallMessage msg;
  msg.epoch = p.GetCellHost(kSanctum).epoch;
  msg.selfId = kTrigger;
  msg.cls = "ObjectReference";
  msg.fn = "Disable";
  msg.args = { Arg(false) };
  msg.caller = "WordWallTriggerBleakFallsScript";
  msg.handler = "onBeginState";
  msg.latent = true;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
}

// the trigger's ref state words sent to `user`, its disabled flags in order
std::vector<bool> TriggerWordsTo(PartOne& p, Networking::UserId user)
{
  std::vector<bool> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<DestructionStageMessage*>(m.message.get());
    if (m.userId == user && typed && typed->refr == kTrigger &&
        typed->disabled) {
      out.push_back(*typed->disabled);
    }
  }
  return out;
}

// the snippets that turn `user`'s copy of the trigger back on (a snippet's
// send is deferred to the next tick, its self the long form id)
size_t TurnedBackOn(PartOne& p, Networking::UserId user)
{
  p.Tick();
  size_t n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j["t"] == static_cast<int>(MsgType::SpSnippet) &&
        m.j["class"] == "ObjectReference" &&
        m.j["function"] == "EnableNoWait" &&
        m.j["selfId"] == SpSnippet::MakeLongFormId(&p.worldState, kTrigger)) {
      ++n;
    }
  }
  return n;
}

void Knows(PartOne& p, uint32_t actor)
{
  p.worldState.GetFormAt<MpActor>(actor).RecordShoutEntry(
    MpActor::ShoutEntry::WordKnown,
    FormDesc::FromFormId(kWord, p.worldState.espmFiles));
}

void Subscribed(PartOne& p)
{
  for (uint32_t id : { kHostActor, kGuestActor }) {
    p.worldState.GetFormAt<MpActor>(id).ForceSubscriptionsUpdate();
  }
  p.Messages().clear();
}

void EnableTrigger(PartOne& p)
{
  auto& trigger = p.worldState.GetFormAt<MpObjectReference>(kTrigger);
  trigger.SetRefState(false, false, trigger.GetRefStateSeq().value_or(0));
}
}

TEST_CASE("The wall's own Disable of its fired trigger reaches as disabled "
          "only the games whose players know the word, and the owner's copy "
          "is turned back on while its own player lacks it",
          "[WordWallOverride][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Subscribed(p);
  // the guest learned first and walked out: its leave ends the wall's play on
  // the host's copy, whose "done" state disables the trigger
  Knows(p, kGuestActor);
  WallDisables(p, 0);
  REQUIRE(p.worldState.GetFormAt<MpObjectReference>(kTrigger).IsDisabled());
  REQUIRE(TriggerWordsTo(p, 1) == std::vector<bool>{ true });
  // the host lacks the word: its own engine's copy is turned back on, so its
  // walk-in reaches the wall
  REQUIRE(TurnedBackOn(p, 0) == 1);
  EnableTrigger(p);
  Leave(p);
}

TEST_CASE("A later player who lacks a fired wall's word is served its "
          "trigger on, though the record holds it disabled",
          "[WordWallOverride][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Subscribed(p);
  Knows(p, kHostActor);
  WallDisables(p, 0);
  REQUIRE(TurnedBackOn(p, 0) == 0);
  // the guest lacks it too: its copy stays on
  REQUIRE(TriggerWordsTo(p, 1) == std::vector<bool>{ false });
  // a late player beside the wall, its subscribe served the trigger's state
  p.Messages().clear();
  DoConnect(p, 2);
  p.CreateActor(kLateActor, kFarInTheSphere, 0, kSanctum);
  p.SetUserActor(2, kLateActor);
  REQUIRE(TriggerWordsTo(p, 2) == std::vector<bool>{ false });
  DoDisconnect(p, 2);
  p.DestroyActor(kLateActor);
  EnableTrigger(p);
  Leave(p);
}

TEST_CASE("The wall's Disable again after a second learner's walk-out turns "
          "off that learner's copy, though the record holds it disabled "
          "already, and leaves the owner who knows the word alone",
          "[WordWallOverride][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Subscribed(p);
  Knows(p, kHostActor);
  WallDisables(p, 0);
  REQUIRE(TriggerWordsTo(p, 1) == std::vector<bool>{ false });
  // the guest learns at its own walk-up, then walks out: the wall's "done"
  // state disables the trigger again on the host's copy
  p.Messages().clear();
  Knows(p, kGuestActor);
  WallDisables(p, 0);
  REQUIRE(TriggerWordsTo(p, 1) == std::vector<bool>{ true });
  REQUIRE(TurnedBackOn(p, 0) == 0);
  EnableTrigger(p);
  Leave(p);
}
