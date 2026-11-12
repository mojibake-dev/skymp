#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <fmt/format.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <vector>

PartOne& GetPartOne();

// thuum ADR-068 rung 5 (ScriptVariables 81; D-V1 B and D-T2 B; docs/research/
// rung5-quests-2026-11-09.md sections 1(b), 7 and 8(b)): any seat reports a
// listed condition-read variable its game changed; the latest write wins
// per key in the server's order (the record's sequence for the key, which
// the relay carries), an equal one folds, and a world key's write goes at
// once to every other seat; a player-class quest's keys are its player's
// own (the player cut): recorded on its actor's change form, relayed to
// nobody, served to it alone, with the quest's other scalar variables and
// its game-time registration's two reserved names. The census's keys: MS13
// 0x39645's ::camillaescort_var (a topic info's write) and PlayerVampireQuest
// 0xeafd5's ::VampireStatus_var (its own script's); LastFeedTime is one of
// its scalar variables (the census's ownVariables; values illustrative).
namespace {
constexpr uint32_t kC1 = 0xff000c61;
constexpr uint32_t kC2 = 0xff000c62;
constexpr uint32_t kMs13 = 0x39645;
constexpr uint32_t kVampire = 0xeafd5;
const char* const kKeys = R"([
  {"form": "39645:Skyrim.esm", "script": "MS13QuestScript", "var": "::camillaescort_var"},
  {"form": "eafd5:Skyrim.esm", "script": "PlayerVampireQuestScript", "var": "::VampireStatus_var"}
])";
const char* const kOwnVariables =
  R"({"eafd5:Skyrim.esm": ["PlayerVampireQuestScript::::LastFeedTime_var"]})";

void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kC1, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kC1);
  DoConnect(p, 1);
  p.CreateActor(kC2, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(1, kC2);
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}

nlohmann::json Entry(uint32_t form, const char* script, const char* var,
                     bool own, uint32_t k, double value)
{
  nlohmann::json e{ { "form", form }, { "script", script }, { "var", var },
                    { "own", own },   { "k", k },           { "seq", 0 } };
  if (k == 3) {
    e["i"] = static_cast<int32_t>(value);
  } else if (k == 4) {
    e["f"] = value;
  } else {
    e["b"] = value != 0;
  }
  return e;
}

void Report(PartOne& p, Networking::UserId user,
            std::vector<nlohmann::json> entries)
{
  DoMessage(
    p, user,
    nlohmann::json{ { "t", static_cast<int>(MsgType::ScriptVariables) },
                    { "watch", nlohmann::json::array() },
                    { "entries", entries },
                    { "served", false } });
}

std::vector<nlohmann::json> VarsTo(PartOne& p, Networking::UserId user)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::ScriptVariables)) {
      out.push_back(m.j);
    }
  }
  return out;
}

struct VarLog
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  VarLog()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("script-vars-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~VarLog() { spdlog::set_default_logger(old); }
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

TEST_CASE("A world key's write is recorded with its sequence and relayed to "
          "the other seat, an equal one folds, an unlisted key is refused",
          "[ScriptVariables][espm]")
{
  PartOne& p = GetPartOne();
  p.SetScriptVariablesSettings(kKeys);
  Enter(p);
  VarLog log;
  Report(
    p, 0,
    { Entry(kMs13, "MS13QuestScript", "::camillaescort_var", false, 3, 1) });
  auto record = p.GetScriptVariables();
  REQUIRE(record.size() == 1);
  REQUIRE(record[0].form == FormDesc::FromString("39645:Skyrim.esm"));
  REQUIRE(record[0].i == std::optional<int32_t>(1));
  REQUIRE(record[0].seq == 1);
  auto relayed = VarsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["served"] == false);
  REQUIRE(relayed[0]["entries"].size() == 1);
  REQUIRE(relayed[0]["entries"][0]["seq"] == 1);
  REQUIRE(relayed[0]["entries"][0]["i"] == 1);
  REQUIRE(VarsTo(p, 0).empty());

  // the other seat writes the same value: folded; a new one: seq 2
  p.Messages().clear();
  Report(
    p, 1,
    { Entry(kMs13, "MS13QuestScript", "::CamillaEscort_var", false, 3, 1) });
  REQUIRE(VarsTo(p, 0).empty());
  Report(
    p, 1,
    { Entry(kMs13, "MS13QuestScript", "::camillaescort_var", false, 3, 2) });
  REQUIRE(p.GetScriptVariables()[0].seq == 2);
  relayed = VarsTo(p, 0);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["entries"][0]["seq"] == 2);

  // an unlisted key, and a kind other than the record's
  p.Messages().clear();
  Report(
    p, 0,
    { Entry(0x74a83, "FreeformRiverwood01Script", "::sent_var", false, 3, 1),
      Entry(kMs13, "MS13QuestScript", "::camillaescort_var", false, 5, 1) });
  REQUIRE(p.GetScriptVariables().size() == 1);
  REQUIRE(VarsTo(p, 1).empty());
  REQUIRE(log.Count("E_SCRIPTVAR_UNLISTED") >= 1);
  REQUIRE(log.Count("E_SCRIPTVAR_KIND") >= 1);
  Leave(p);
}

TEST_CASE("A player-class quest's keys are its player's own: the world's "
          "report refused (E_SCRIPTVAR_PLAYER), the own one recorded on its "
          "actor and relayed to nobody, its registration's -1 kept",
          "[ScriptVariables][espm]")
{
  PartOne& p = GetPartOne();
  p.SetScriptVariablesSettings(kKeys);
  p.SetPlayerQuestSettings("{}", kOwnVariables, "{}");
  p.SetQuestScopeSettings(R"("derived")", R"(["eafd5:Skyrim.esm"])");
  Enter(p);
  VarLog log;
  Report(p, 0,
         { Entry(kVampire, "PlayerVampireQuestScript", "::VampireStatus_var",
                 false, 3, 2) });
  // the refusal's own line (the report's line counts it too)
  REQUIRE(log.Count("E_SCRIPTVAR_PLAYER: ff000c61 reports script variable") ==
          1);
  REQUIRE(log.Count(", E_SCRIPTVAR_PLAYER x1") == 1);
  REQUIRE(p.GetScriptVariables().empty());
  Report(p, 0,
         { Entry(kVampire, "PlayerVampireQuestScript", "::VampireStatus_var",
                 true, 3, 2),
           Entry(kVampire, "PlayerVampireQuestScript", "::LastFeedTime_var",
                 true, 4, 4.5),
           Entry(kVampire, "PlayerVampireQuestScript", "<OnUpdateGameTime>",
                 true, 4, -1),
           Entry(kVampire, "PlayerVampireQuestScript",
                 "<OnUpdateGameTime:repeat>", true, 4, 0) });
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  const auto own = c1.GetScriptVariables();
  REQUIRE(own.size() == 4);
  REQUIRE(own[2].var == "<OnUpdateGameTime>");
  REQUIRE(own[2].f == std::optional<float>(-1.f));
  REQUIRE(p.GetScriptVariables().empty());
  REQUIRE(VarsTo(p, 1).empty());
  REQUIRE(VarsTo(p, 0).empty());

  // the login serves the watch with the own flags and the own record to
  // its player alone, the pair of the registration in one message
  p.Messages().clear();
  p.SetUserActor(0, kC1);
  const auto served = VarsTo(p, 0);
  REQUIRE(served.size() == 2);
  // the two listed keys first (the escort the world's, the status the
  // player's own), then the own keys: the vampire's LastFeedTime and every
  // player-class quest's registration pair (the masters mark four), each own
  const auto& watch = served[0]["watch"];
  REQUIRE(watch.size() >= 5);
  REQUIRE(watch[0]["var"] == "::camillaescort_var");
  REQUIRE(watch[0]["own"] == false);
  REQUIRE(watch[1]["var"] == "::VampireStatus_var");
  REQUIRE(watch[1]["own"] == true);
  const auto watched = [&](uint32_t form, const char* var) {
    return std::count_if(watch.begin(), watch.end(), [&](const auto& key) {
             return key["form"] == form && key["var"] == var &&
               key["own"] == true;
           }) == 1;
  };
  REQUIRE(watched(kVampire, "::LastFeedTime_var"));
  REQUIRE(watched(kVampire, "<OnUpdateGameTime>"));
  REQUIRE(watched(kVampire, "<OnUpdateGameTime:repeat>"));
  size_t reserved = 0;
  for (size_t i = 1; i < watch.size(); ++i) {
    REQUIRE(watch[i]["own"] == true);
    reserved +=
      watch[i]["var"].get<std::string>().rfind("<OnUpdateGameTime", 0) == 0
      ? 1
      : 0;
  }
  REQUIRE(reserved % 2 == 0);
  REQUIRE(watch.size() == 3 + reserved);
  REQUIRE(served[1]["served"] == true);
  REQUIRE(served[1]["entries"].size() == 4);
  REQUIRE(served[1]["entries"][0]["own"] == true);
  p.Messages().clear();
  p.SetUserActor(1, kC2);
  const auto other = VarsTo(p, 1);
  REQUIRE(other.size() == 1); // its watch, none of c1's own
  Leave(p);
}

TEST_CASE("The login's watch and record go after the world globals, at most "
          "256 a message in order, a registration's pair never split, and "
          "survive a restart",
          "[ScriptVariables][espm]")
{
  PartOne& p = GetPartOne();
  // 300 listed keys: the census's 798 do not fit one message either
  std::string keys = "[";
  for (int n = 0; n < 300; ++n) {
    keys += fmt::format(
      R"({}{{"form": "39645:Skyrim.esm", "script": "MS13QuestScript", "var": "::v{}_var"}})",
      n == 0 ? "" : ",", n);
  }
  keys += "]";
  p.SetScriptVariablesSettings(keys);
  // 255 of the vampire quest's own variables, so its registration's pair
  // would straddle the first message's end
  std::string vars = R"({"eafd5:Skyrim.esm": [)";
  for (int n = 0; n < 255; ++n) {
    vars += fmt::format(R"({}"PlayerVampireQuestScript::::own{}_var")",
                        n == 0 ? "" : ",", n);
  }
  vars += "]}";
  p.SetPlayerQuestSettings("{}", vars, "{}");
  p.SetQuestScopeSettings(R"("derived")", R"(["eafd5:Skyrim.esm"])");
  p.SetWorldGlobalsEnabled(true);
  p.SetWorldGlobals({ { FormDesc::FromString("17424:Skyrim.esm"), 2.f } });
  Enter(p);

  // the player's own record: the 255 variables, then the pair
  std::vector<MpChangeFormREFR::ScriptVariable> own;
  for (int n = 0; n < 255; ++n) {
    MpChangeFormREFR::ScriptVariable v;
    v.form = FormDesc::FromString("eafd5:Skyrim.esm");
    v.script = "PlayerVampireQuestScript";
    v.var = fmt::format("::own{}_var", n);
    v.k = 3;
    v.i = n;
    v.seq = 1;
    own.push_back(v);
  }
  for (const char* name :
       { "<OnUpdateGameTime>", "<OnUpdateGameTime:repeat>" }) {
    MpChangeFormREFR::ScriptVariable v;
    v.form = FormDesc::FromString("eafd5:Skyrim.esm");
    v.script = "PlayerVampireQuestScript";
    v.var = name;
    v.k = 4;
    v.f = 1.f;
    v.seq = 1;
    own.push_back(v);
  }
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  REQUIRE(c1.SetScriptVariables(own));

  p.Messages().clear();
  p.SetUserActor(0, kC1);
  size_t lastGlobal = 0;
  size_t firstVars = 0;
  std::vector<size_t> watches;
  std::vector<size_t> entries;
  size_t reserved = 0;
  bool vampirePair = false;
  size_t at = 0;
  for (auto& m : p.Messages()) {
    ++at;
    if (m.userId != 0 || !m.j.is_object()) {
      continue;
    }
    if (m.j.value("t", -1) == static_cast<int>(MsgType::WorldGlobal)) {
      lastGlobal = at;
    }
    if (m.j.value("t", -1) == static_cast<int>(MsgType::ScriptVariables)) {
      firstVars = firstVars ? firstVars : at;
      if (!m.j["watch"].empty()) {
        watches.push_back(m.j["watch"].size());
        for (const auto& key : m.j["watch"]) {
          if (key["var"].get<std::string>().rfind("<OnUpdateGameTime", 0) ==
              0) {
            ++reserved;
            vampirePair = vampirePair || key["form"] == kVampire;
          }
        }
      }
      if (!m.j["entries"].empty()) {
        entries.push_back(m.j["entries"].size());
        // the pair sits in one message
        size_t pair = 0;
        for (const auto& e : m.j["entries"]) {
          pair +=
            e["var"].get<std::string>().rfind("<OnUpdateGameTime", 0) == 0 ? 1
                                                                           : 0;
        }
        REQUIRE((pair == 0 || pair == 2));
      }
    }
  }
  REQUIRE(lastGlobal > 0);
  REQUIRE(firstVars > lastGlobal);
  // 300 listed keys and the own ones: the vampire quest's 255 variables and
  // every player-class quest's registration pair (each marked quest with a
  // script has one), in messages of 256 but the last
  REQUIRE(vampirePair);
  REQUIRE(reserved >= 2);
  REQUIRE(reserved % 2 == 0);
  size_t total = 0;
  for (size_t i = 0; i < watches.size(); ++i) {
    total += watches[i];
    if (i + 1 < watches.size()) {
      REQUIRE(watches[i] == 256);
    }
  }
  REQUIRE(total == 300 + 255 + reserved);
  REQUIRE(watches.size() == (total + 255) / 256);
  // the own record: 255 alone, the pair in the next message
  REQUIRE(entries == std::vector<size_t>{ 255, 2 });

  // a restart keeps the journal's world keys and the player's own record
  Report(p, 0, { Entry(kMs13, "MS13QuestScript", "::v7_var", false, 3, 7) });
  const MpChangeForm journal = p.WorldJournal().GetChangeForm();
  const MpChangeForm player = c1.GetChangeForm();
  REQUIRE(journal.scriptVariables.has_value());
  REQUIRE(player.scriptVariables == own);
  Leave(p);
  PartOne& restarted = GetPartOne();
  restarted.worldState.LoadChangeForm(journal,
                                      restarted.CreateFormCallbacks());
  REQUIRE(restarted.GetScriptVariables() == *journal.scriptVariables);
}
