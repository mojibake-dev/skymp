#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <simdjson.h>
#include <vector>

PartOne& GetPartOne();

// thuum ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md section
// 1(b), rule 6): the world's quest runs and the condition-read script
// variables live in change forms, the runs and the world's variables on the
// world's journal and a player's own variables on its actor, named by
// FormDesc; a record from before either field reads as none and keeps its
// formVersion. Skyrim.esm's FreeformRiverwood01 0x74a83 (A Lovely Letter)
// and the plan's x-quest-words quest 0x10d3bb; MS13 0x39645's
// ::camillaescort_var (a topic info's write, the census's) and
// PlayerVampireQuest 0xeafd5's VampireStatus with its game-time
// registration (a player's own; the values illustrative)

namespace {
MpChangeFormREFR::ScriptVariable Var(const char* form, const char* script,
                                     const char* var, uint32_t k, uint32_t seq)
{
  MpChangeFormREFR::ScriptVariable v;
  v.form = FormDesc::FromString(form);
  v.script = script;
  v.var = var;
  v.k = k;
  v.seq = seq;
  return v;
}

MpChangeForm RoundTrip(const MpChangeForm& changeForm)
{
  static simdjson::dom::parser parser;
  static std::string dump;
  dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  return MpChangeForm::JsonToChangeForm(element);
}
}

TEST_CASE("The change form keeps the world's quest runs and the script "
          "variables, and older records read as none",
          "[QuestRun]")
{
  MpChangeForm changeForm;
  changeForm.questRuns = std::vector<MpChangeFormREFR::QuestRun>{
    { FormDesc::FromString("74a83:Skyrim.esm"), true, 1 },
    { FormDesc::FromString("10d3bb:Skyrim.esm"), false, 2 }
  };
  auto escort =
    Var("39645:Skyrim.esm", "MS13QuestScript", "::camillaescort_var", 3, 2);
  escort.i = 1;
  auto status = Var("eafd5:Skyrim.esm", "PlayerVampireQuestScript",
                    "::VampireStatus_var", 3, 1);
  status.i = 2;
  auto hours = Var("eafd5:Skyrim.esm", "PlayerVampireQuestScript",
                   "<OnUpdateGameTime>", 4, 3);
  // an ended registration, recorded as -1 so a login registers nothing
  hours.f = -1.f;
  auto cured = Var("eafd5:Skyrim.esm", "PlayerVampireQuestScript",
                   "::IsVampire_var", 5, 1);
  cured.b = false;
  changeForm.scriptVariables =
    std::vector<MpChangeFormREFR::ScriptVariable>{ escort, status, hours,
                                                   cured };

  const nlohmann::json saved = MpChangeForm::ToJson(changeForm);
  REQUIRE(saved["questRuns"]["entries"].size() == 2);
  REQUIRE(saved["questRuns"]["entries"][0]["quest"] == "74a83:Skyrim.esm");
  REQUIRE(saved["questRuns"]["entries"][0]["running"] == true);
  REQUIRE(saved["questRuns"]["entries"][1]["seq"] == 2);
  const auto& vars = saved["scriptVariables"]["entries"];
  REQUIRE(vars.size() == 4);
  // the one value the kind names, and no other
  REQUIRE(vars[0]["i"] == 1);
  REQUIRE(!vars[0].contains("f"));
  REQUIRE(!vars[0].contains("b"));
  REQUIRE(vars[2]["f"] == -1.f);
  REQUIRE(vars[3]["b"] == false);

  const MpChangeForm loaded = RoundTrip(changeForm);
  REQUIRE(loaded.questRuns == changeForm.questRuns);
  REQUIRE(loaded.scriptVariables == changeForm.scriptVariables);
  REQUIRE(loaded.formVersion == changeForm.formVersion);

  // rule 6's migration: a record from before the fields has neither key and
  // reads as none
  const nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("questRuns"));
  REQUIRE(!older.contains("scriptVariables"));
  const MpChangeForm olderLoaded = RoundTrip(MpChangeForm());
  REQUIRE(!olderLoaded.questRuns);
  REQUIRE(!olderLoaded.scriptVariables);
}

TEST_CASE("The world's quest runs and script variables survive a restart "
          "through the journal's record, and a player's own variables "
          "through its actor's",
          "[QuestRun][espm]")
{
  PartOne& running = GetPartOne();
  // a read makes no journal
  REQUIRE(running.GetQuestRuns().empty());
  REQUIRE(running.GetScriptVariables().empty());
  REQUIRE(!running.SetQuestRuns({}));
  REQUIRE(!running.worldState.LookupFormById(PartOne::kWorldJournalRefId));

  const std::vector<MpChangeFormREFR::QuestRun> runs = {
    { FormDesc::FromString("10d3bb:Skyrim.esm"), true, 1 }
  };
  auto escort =
    Var("39645:Skyrim.esm", "MS13QuestScript", "::camillaescort_var", 3, 1);
  escort.i = 1;
  const std::vector<MpChangeFormREFR::ScriptVariable> world = { escort };
  REQUIRE(running.SetQuestRuns(runs));
  REQUIRE(running.SetScriptVariables(world));
  REQUIRE(!running.SetQuestRuns(runs)); // unchanged
  REQUIRE(running.GetQuestRuns() == runs);
  REQUIRE(running.GetScriptVariables() == world);

  // a player's own, on its actor
  constexpr uint32_t kPlayer = 0xff000c51;
  running.CreateActor(kPlayer, { 0, 0, 0 }, 0, 0x3c);
  auto& player = running.worldState.GetFormAt<MpActor>(kPlayer);
  auto status = Var("eafd5:Skyrim.esm", "PlayerVampireQuestScript",
                    "::VampireStatus_var", 3, 1);
  status.i = 2;
  const std::vector<MpChangeFormREFR::ScriptVariable> own = { status };
  REQUIRE(player.SetScriptVariables(own));
  const MpChangeForm savedPlayer = player.GetChangeForm();
  REQUIRE(RoundTrip(savedPlayer).scriptVariables == own);

  const MpChangeForm saved = running.WorldJournal().GetChangeForm();
  REQUIRE(saved.questRuns == runs);
  REQUIRE(saved.scriptVariables == world);
  running.DestroyActor(kPlayer);

  // the restarted server reads both records back
  PartOne& restarted = GetPartOne();
  restarted.worldState.LoadChangeForm(RoundTrip(saved),
                                      restarted.CreateFormCallbacks());
  REQUIRE(restarted.GetQuestRuns() == runs);
  REQUIRE(restarted.GetScriptVariables() == world);
}
