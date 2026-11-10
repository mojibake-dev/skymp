#include "ActionListener.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include "libespm/Convert.h"
#include "libespm/QUST.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <fmt/format.h>
#include <simdjson.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <tuple>
#include <vector>

PartOne& GetPartOne();

// thuum docs/verbs/quest-stage.md (ADR-056): the world's quest stages live in
// the world's journal reference's change form, named by FormDesc, and a
// record from before the field reads as none (rule 6's migration).

TEST_CASE("The change form keeps the world's quest stages, and older records "
          "read as none",
          "[QuestStage]")
{
  MpChangeForm changeForm;
  // Skyrim.esm QUST 0x39645 MS13 (thuum lab/esm.py), two of its stages
  changeForm.questStages = { { FormDesc::FromString("39645:Skyrim.esm"), 10 },
                             { FormDesc::FromString("39645:Skyrim.esm"),
                               20 } };

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).questStages ==
          changeForm.questStages);

  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("questStages"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).questStages);
}

// The handler, the relay, the login and the restart, on Skyrim.esm's own
// records (thuum lab/esm.py on the 1.6.1170 master): QUST 0x39645 MS13 is
// the listed quest (its stage indexes include 5, 10 and 20) and the
// start-game-enabled one (DNAM main flags 0x11; MQ101 0x3372b reads 0x00
// there, not Start Game Enabled, so MS13 stands for memo D62's rule), QUST
// 0xb6031 MS13Intro a quest off the list, DNAM 0x00 (in mode all, memo D58
// B, a quest nobody declared), QUST 0x4e50d MQ102 the quest mode all
// denies, DNAM 0x00, STAT 0x3b XMarker no quest at all.
namespace {
constexpr uint32_t kReporter = 0xff000bd0;
constexpr uint32_t kWatcher = 0xff000bd1;
constexpr uint32_t kMs13 = 0x39645;
constexpr uint32_t kMs13Intro = 0xb6031;
constexpr uint32_t kMq102 = 0x4e50d;
// FreeformHeartwoodMill: start-game-enabled, its INDX 10 a start-up stage and
// its 20 a plain one (a misc objective's shape, the rumor's), no stage 0
// (lab/esm.py on sky-srv, 2026-11-10)
constexpr uint32_t kHeartwoodMill = 0xe3ea5;
constexpr uint32_t kXMarker = 0x3b;
// a form of a sixth plugin, which the test's five-file load order lacks
constexpr uint32_t kUnloaded = 0x05000d62;
const char* const kSharedQuests = R"(["39645:Skyrim.esm"])";
const char* const kDeniedQuests =
  R"({"mode":"all","denied":["4e50d:Skyrim.esm"]})";
const char* const kAllowedQuests =
  R"({"mode":"all","denied":["4e50d:Skyrim.esm"],"allowed":["39645:Skyrim.esm"]})";

// one QuestStage message as sent: the quest, the stage, the shared list,
// the mode (0 list, 1 all)
using Sent = std::tuple<uint32_t, int, nlohmann::json, int>;

Sent Msg(uint32_t quest, int stage,
         nlohmann::json shared = nlohmann::json::array(), int mode = 0)
{
  return Sent{ quest, stage, std::move(shared), mode };
}

void Enter(PartOne& p, const char* sharedQuests = kSharedQuests)
{
  p.SetSharedQuestsSettings(sharedQuests);
  // the test server is shared across cases: each case starts from an empty
  // world record
  p.SetQuestStages({});
  DoConnect(p, 0);
  p.CreateActor(kReporter, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kReporter);
  DoConnect(p, 1);
  p.CreateActor(kWatcher, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(1, kWatcher);
  p.Messages().clear();
}

void Report(PartOne& p, Networking::UserId user, uint32_t quest, int stage)
{
  DoMessage(p, user,
            nlohmann::json{ { "t", static_cast<int>(MsgType::QuestStage) },
                            { "quest", quest },
                            { "stage", stage },
                            { "shared", nlohmann::json::array() },
                            { "mode", 0 } });
}

std::vector<Sent> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<Sent> sent;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::QuestStage)) {
      sent.emplace_back(m.j.value("quest", 0u), m.j.value("stage", -1),
                        m.j.contains("shared") ? m.j["shared"]
                                               : nlohmann::json::array(),
                        m.j.value("mode", 0));
    }
  }
  return sent;
}

void Leave(PartOne& p)
{
  p.SetSharedQuestsSettings("[]");
  p.DestroyActor(kReporter);
  p.DestroyActor(kWatcher);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}
}

TEST_CASE("A listed quest's stage is recorded as the world's and relayed to "
          "the other player, not the reporter; the same stage again is a "
          "no-op",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);

  Report(p, 0, kMs13, 10);
  const FormDesc ms13 = FormDesc::FromString("39645:Skyrim.esm");
  REQUIRE(p.GetQuestStages() == std::vector<QuestStageDone>{ { ms13, 10 } });
  REQUIRE(SentTo(p, 1) == std::vector<Sent>{ Msg(kMs13, 10) });
  REQUIRE(SentTo(p, 0).empty());

  // the same stage again: the record unchanged, nothing relayed
  p.Messages().clear();
  Report(p, 0, kMs13, 10);
  REQUIRE(p.GetQuestStages() == std::vector<QuestStageDone>{ { ms13, 10 } });
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(SentTo(p, 0).empty());

  // the other player's stage, set out of order, joins the record ascending
  // within the quest and reaches the first player alone
  p.Messages().clear();
  Report(p, 1, kMs13, 5);
  REQUIRE(p.GetQuestStages() ==
          std::vector<QuestStageDone>{ { ms13, 5 }, { ms13, 10 } });
  REQUIRE(SentTo(p, 0) == std::vector<Sent>{ Msg(kMs13, 5) });
  REQUIRE(SentTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("A stage of a form in no loaded plugin, of a form that is no "
          "quest, or of a quest off the shared list is refused and records "
          "nothing; so is a client's report naming the list, a mode or "
          "quest 0",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Report(p, 0, kUnloaded, 10);
  Report(p, 0, kXMarker, 10);
  Report(p, 0, kMs13Intro, 10);
  Report(p, 0, 0, 10);
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::QuestStage) },
                            { "quest", kMs13 },
                            { "stage", 10 },
                            { "shared", nlohmann::json::array({ kMs13 }) },
                            { "mode", 0 } });
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::QuestStage) },
                            { "quest", kMs13 },
                            { "stage", 10 },
                            { "shared", nlohmann::json::array() },
                            { "mode", 1 } });
  REQUIRE(p.GetQuestStages().empty());
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(SentTo(p, 0).empty());
  Leave(p);
}

// the review of m3-pass-train-2, finding 2 (rule 5): the stage index is
// checked against the quest's own INDX list, read from the same browser the
// handler reads
TEST_CASE("A stage the quest's record does not define is refused "
          "(E_QUEST_STAGE) and records nothing; a stage it defines passes",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  const auto qust =
    espm::Convert<espm::QUST>(p.GetEspm().GetBrowser().LookupById(kMs13).rec);
  REQUIRE(qust);
  std::vector<uint16_t> indexes;
  for (const auto& stage : qust->GetQuestStages(p.worldState.GetEspmCache())) {
    REQUIRE(stage.journalIndex);
    indexes.push_back(static_cast<uint16_t>(stage.journalIndex->index));
  }
  REQUIRE(std::find(indexes.begin(), indexes.end(), 10) != indexes.end());
  REQUIRE(std::find(indexes.begin(), indexes.end(), 9999) == indexes.end());

  Report(p, 0, kMs13, 9999);
  REQUIRE(p.GetQuestStages().empty());
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(SentTo(p, 0).empty());

  Report(p, 0, kMs13, 10);
  REQUIRE(p.GetQuestStages() ==
          std::vector<QuestStageDone>{
            { FormDesc::FromString("39645:Skyrim.esm"), 10 } });
  REQUIRE(SentTo(p, 1) == std::vector<Sent>{ Msg(kMs13, 10) });
  Leave(p);
}

// finding 2 (rule 12): the world's record is bounded at wire-rules
// quest_stages' QUEST_RECORD_CAP, 4096 pairs; the record is filled directly
// (no INDX check on the record itself, an older record's stages stay)
TEST_CASE("A report past the record's cap is refused (E_QUEST_FULL) and "
          "records nothing; a stage already in the full record is still a "
          "no-op; one below the cap takes one more stage",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  const FormDesc ms13 = FormDesc::FromString("39645:Skyrim.esm");
  std::vector<QuestStageDone> full;
  for (uint16_t stage = 11; stage <= 4106; ++stage) {
    full.push_back({ ms13, stage });
  }
  REQUIRE(full.size() == 4096);
  p.SetQuestStages(full);

  // MS13's stage 10 is a stage the quest defines and the record lacks
  Report(p, 0, kMs13, 10);
  REQUIRE(p.GetQuestStages() == full);
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(SentTo(p, 0).empty());

  // a stage in the record already: a no-op, as below the cap
  Report(p, 0, kMs13, 20);
  REQUIRE(p.GetQuestStages() == full);
  REQUIRE(SentTo(p, 1).empty());

  // one below the cap: the stage joins the record, which is then full
  full.pop_back();
  p.SetQuestStages(full);
  Report(p, 0, kMs13, 10);
  REQUIRE(p.GetQuestStages().size() == 4096);
  REQUIRE(p.QuestStageDoneAlready({ ms13, 10 }));
  REQUIRE(SentTo(p, 1) == std::vector<Sent>{ Msg(kMs13, 10) });
  p.Messages().clear();
  Report(p, 0, kMs13, 5);
  REQUIRE(p.GetQuestStages().size() == 4096);
  REQUIRE(!p.QuestStageDoneAlready({ ms13, 5 }));
  REQUIRE(SentTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("The login replays the shared list, then the world's stages "
          "ascending per quest",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  Report(p, 0, kMs13, 20);
  Report(p, 0, kMs13, 10);

  // a second login of the same actor is what a reconnect does
  p.Messages().clear();
  p.SetUserActor(1, kWatcher);
  REQUIRE(SentTo(p, 1) ==
          std::vector<Sent>{ Msg(0, 0, nlohmann::json::array({ kMs13 })),
                             Msg(kMs13, 10), Msg(kMs13, 20) });
  REQUIRE(SentTo(p, 0).empty());
  Leave(p);
}

// memo D58 B (2026-10-29): sharing mode all with a denylist
TEST_CASE("In mode all a quest nobody declared is the world's: its stage is "
          "recorded and relayed; a denied quest's stage is refused "
          "(E_QUEST_DENIED); the login names mode 1 and the denied quests, "
          "none or not, then the record",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);

  // MS13Intro is on no list: recorded and relayed all the same
  Report(p, 0, kMs13Intro, 10);
  const FormDesc ms13Intro = FormDesc::FromString("b6031:Skyrim.esm");
  REQUIRE(p.GetQuestStages() ==
          std::vector<QuestStageDone>{ { ms13Intro, 10 } });
  REQUIRE(SentTo(p, 1) == std::vector<Sent>{ Msg(kMs13Intro, 10) });
  REQUIRE(SentTo(p, 0).empty());

  // MQ102 is denied: refused, nothing recorded, nothing relayed
  p.Messages().clear();
  Report(p, 0, kMq102, 10);
  REQUIRE(p.GetQuestStages() ==
          std::vector<QuestStageDone>{ { ms13Intro, 10 } });
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(SentTo(p, 0).empty());

  // the login: mode 1 with the denied quest, then the world's stages
  p.Messages().clear();
  p.SetUserActor(1, kWatcher);
  REQUIRE(SentTo(p, 1) ==
          std::vector<Sent>{ Msg(0, 0, nlohmann::json::array({ kMq102 }), 1),
                             Msg(kMs13Intro, 10) });
  REQUIRE(SentTo(p, 0).empty());

  // nothing denied: the login still names the mode, with an empty list
  p.SetSharedQuestsSettings(R"({"mode":"all"})");
  p.Messages().clear();
  p.SetUserActor(1, kWatcher);
  REQUIRE(SentTo(p, 1) ==
          std::vector<Sent>{ Msg(0, 0, nlohmann::json::array(), 1),
                             Msg(kMs13Intro, 10) });

  // a block of another shape stops at the setting, named, and the setting
  // in force stays
  REQUIRE_THROWS(p.SetSharedQuestsSettings(R"({"mode":"list"})"));
  REQUIRE(p.SharedQuestsMode() == 1);
  Leave(p);
}

// thuum ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md sections
// 1(b) and 8(a)): a quest's run state is the world's too. A start or a stop
// (QuestStage's `word` 1 or 2; a report with no word of stage 0 or of a
// start-up stage is a start, read 5, generalized 2026-11-10) is recorded on
// the journal's questRuns, folded when it equals the record and relayed with
// the acting player; memos D62 and D69 retired with the start word, so a
// start-game-enabled quest's own start is recorded once instead of refused
// (E_QUEST_OWN_START is gone), and its start-up stage's report is that start
// too, never a stage (b-quest-rumor's HeartwoodMill claim).
namespace {
// the plan's x-quest-words quest, a script-started one (Skyrim.esm)
constexpr uint32_t kLetterQuest = 0x10d3bb;

// one QuestStage message as sent, rung 5's fields too: the quest, the
// stage, the word (0 none), the acting player (0 none), served
using Word = std::tuple<uint32_t, int, uint32_t, uint32_t, bool>;

Word W(uint32_t quest, int stage, uint32_t word, uint32_t acting,
       bool served = false)
{
  return Word{ quest, stage, word, acting, served };
}

std::vector<Word> WordsTo(PartOne& p, Networking::UserId user)
{
  std::vector<Word> sent;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::QuestStage) &&
        m.j.value("quest", 0u) != 0) {
      sent.emplace_back(m.j.value("quest", 0u), m.j.value("stage", -1),
                        m.j.value("word", 0u), m.j.value("acting", 0u),
                        m.j.value("served", false));
    }
  }
  return sent;
}

void ReportWord(PartOne& p, Networking::UserId user, uint32_t quest,
                uint32_t word, std::optional<uint32_t> acting = std::nullopt)
{
  nlohmann::json j{ { "t", static_cast<int>(MsgType::QuestStage) },
                    { "quest", quest },
                    { "stage", 0 },
                    { "shared", nlohmann::json::array() },
                    { "mode", 0 },
                    { "word", word } };
  if (acting) {
    j["acting"] = *acting;
  }
  DoMessage(p, user, j);
}

void ReportActing(PartOne& p, Networking::UserId user, uint32_t quest,
                  int stage, uint32_t acting)
{
  DoMessage(p, user,
            nlohmann::json{ { "t", static_cast<int>(MsgType::QuestStage) },
                            { "quest", quest },
                            { "stage", stage },
                            { "shared", nlohmann::json::array() },
                            { "mode", 0 },
                            { "acting", acting } });
}

// the default logger's lines at info and above while the guard lives
// (ScriptCallTest's shape), restored at its end
struct QuestLog
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  QuestLog()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("quest-stage-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~QuestLog() { spdlog::set_default_logger(old); }
  // a line's end is "\n" here whatever the platform's: spdlog ends a line
  // with SPDLOG_EOL, "\r\n" on Windows (the mirror's Windows ctest read 0
  // for a count that ends its line), so the carriage returns are dropped
  size_t Count(const std::string& line) const
  {
    std::string log = oss.str();
    log.erase(std::remove(log.begin(), log.end(), '\r'), log.end());
    size_t n = 0;
    for (size_t at = log.find(line); at != std::string::npos;
         at = log.find(line, at + line.size())) {
      ++n;
    }
    return n;
  }
};

std::vector<MpChangeFormREFR::QuestRun> Runs(
  std::initializer_list<std::tuple<const char*, bool, uint32_t>> rows)
{
  std::vector<MpChangeFormREFR::QuestRun> runs;
  for (const auto& [quest, running, seq] : rows) {
    runs.push_back({ FormDesc::FromString(quest), running, seq });
  }
  return runs;
}
}

TEST_CASE("The retired allowed list is read and ignored: a quest it names is "
          "the world's, one both denied and allowed denied",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  REQUIRE_NOTHROW(p.SetSharedQuestsSettings(kAllowedQuests));
  REQUIRE(p.SharedQuestsMode() == 1);
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("39645:Skyrim.esm")) == 0);
  REQUIRE_NOTHROW(p.SetSharedQuestsSettings(
    R"({"mode":"all","denied":["39645:Skyrim.esm"],"allowed":["39645:Skyrim.esm"]})"));
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("39645:Skyrim.esm")) == 2);
  p.SetSharedQuestsSettings("[]");
}

TEST_CASE("A start-game-enabled quest's stage 0 is the start word's: its run "
          "is recorded once and relayed, no stage and no E_QUEST_ line; its "
          "start-up stage is the start word's too, folded into the run",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);
  QuestLog log;
  // FreeformHeartwoodMill is Start Game Enabled; its engine's own start
  // reports stage 0 with no word
  Report(p, 0, kHeartwoodMill, 0);
  REQUIRE(p.GetQuestStages().empty());
  REQUIRE(p.GetQuestRuns() == Runs({ { "e3ea5:Skyrim.esm", true, 1 } }));
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{ W(kHeartwoodMill, 0, 1, kReporter) });
  REQUIRE(WordsTo(p, 0).empty());
  REQUIRE(log.Count("[verb:quest-stage] ff000bd0 starts e3ea5:Skyrim.esm: "
                    "recorded, relayed to 1") == 1);
  REQUIRE(log.Count("E_QUEST_") == 0);

  // the other seat's engine starts it too: folded, nothing relayed
  p.Messages().clear();
  Report(p, 1, kHeartwoodMill, 0);
  REQUIRE(p.GetQuestRuns() == Runs({ { "e3ea5:Skyrim.esm", true, 1 } }));
  REQUIRE(WordsTo(p, 0).empty());
  REQUIRE(WordsTo(p, 1).empty());

  // its start-up stage 10 from either seat (read 5, generalized 2026-11-10):
  // the start word's, folded into the recorded run; no stage, no set line,
  // nothing relayed
  Report(p, 0, kHeartwoodMill, 10);
  Report(p, 1, kHeartwoodMill, 10);
  REQUIRE(p.GetQuestStages().empty());
  REQUIRE(p.GetQuestRuns() == Runs({ { "e3ea5:Skyrim.esm", true, 1 } }));
  REQUIRE(WordsTo(p, 0).empty());
  REQUIRE(WordsTo(p, 1).empty());
  REQUIRE(log.Count("set e3ea5:Skyrim.esm stage 10") == 0);
  REQUIRE(log.Count("E_QUEST_") == 0);
  Leave(p);
}

// read 5, generalized (2026-11-10; b-quest-rumor's HeartwoodMill claim,
// 'server.log_count("set e3ea5:Skyrim.esm stage 10") == 0', kept by its v2):
// a seat whose first report of a quest is its start-up stage records the
// quest's start, relayed as the start word with its engine stages and never
// as a stage; the quest's unflagged stage after it crosses as before
TEST_CASE("A start-up stage reported first records the quest's start, "
          "relayed as the start word, no stage and no set line; an unflagged "
          "stage after it is recorded and relayed like any other",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);
  QuestLog log;
  Report(p, 0, kHeartwoodMill, 10);
  REQUIRE(p.GetQuestStages().empty());
  REQUIRE(p.GetQuestRuns() == Runs({ { "e3ea5:Skyrim.esm", true, 1 } }));
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{ W(kHeartwoodMill, 0, 1, kReporter) });
  REQUIRE(WordsTo(p, 0).empty());
  REQUIRE(log.Count("[verb:quest-stage] ff000bd0 starts e3ea5:Skyrim.esm: "
                    "recorded, relayed to 1 with its engine stages "
                    "10\n") == 1);
  REQUIRE(log.Count("set e3ea5:Skyrim.esm stage 10") == 0);

  // the other seat's start word folds into it, nothing relayed
  p.Messages().clear();
  ReportWord(p, 1, kHeartwoodMill, 1);
  REQUIRE(p.GetQuestRuns() == Runs({ { "e3ea5:Skyrim.esm", true, 1 } }));
  REQUIRE(WordsTo(p, 0).empty());
  REQUIRE(WordsTo(p, 1).empty());

  // stage 20 carries no flag: a stage like any other, recorded and relayed
  Report(p, 0, kHeartwoodMill, 20);
  const FormDesc mill = FormDesc::FromString("e3ea5:Skyrim.esm");
  REQUIRE(p.GetQuestStages() == std::vector<QuestStageDone>{ { mill, 20 } });
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{ W(kHeartwoodMill, 20, 0, kReporter) });
  REQUIRE(log.Count("[verb:quest-stage] ff000bd0 set e3ea5:Skyrim.esm stage "
                    "20: recorded, relayed to 1") == 1);
  REQUIRE(log.Count("set e3ea5:Skyrim.esm stage 10") == 0);
  REQUIRE(log.Count("E_QUEST_") == 0);
  Leave(p);
}

TEST_CASE("A start is recorded and relayed with the reporter acting, a "
          "repeated start folds, a stop is relayed; a denied quest's word "
          "is refused (E_QUEST_DENIED)",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);
  QuestLog log;
  ReportWord(p, 0, kLetterQuest, 1);
  REQUIRE(p.GetQuestRuns() == Runs({ { "10d3bb:Skyrim.esm", true, 1 } }));
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{ W(kLetterQuest, 0, 1, kReporter) });
  REQUIRE(WordsTo(p, 0).empty());

  p.Messages().clear();
  ReportWord(p, 0, kLetterQuest, 1);
  ReportWord(p, 1, kLetterQuest, 1);
  REQUIRE(p.GetQuestRuns() == Runs({ { "10d3bb:Skyrim.esm", true, 1 } }));
  REQUIRE(WordsTo(p, 0).empty());
  REQUIRE(WordsTo(p, 1).empty());

  ReportWord(p, 1, kLetterQuest, 2);
  REQUIRE(p.GetQuestRuns() == Runs({ { "10d3bb:Skyrim.esm", false, 2 } }));
  REQUIRE(WordsTo(p, 0) ==
          std::vector<Word>{ W(kLetterQuest, 0, 2, kWatcher) });
  REQUIRE(log.Count("[verb:quest-stage] ff000bd1 stops 10d3bb:Skyrim.esm: "
                    "recorded, relayed to 1") == 1);

  // MQ102 is denied: its start and its stage refused, nothing written
  p.Messages().clear();
  ReportWord(p, 0, kMq102, 1);
  Report(p, 0, kMq102, 10);
  REQUIRE(p.GetQuestRuns() == Runs({ { "10d3bb:Skyrim.esm", false, 2 } }));
  REQUIRE(p.GetQuestStages().empty());
  REQUIRE(WordsTo(p, 1).empty());
  // the word by the setting's word; the stage too, unless MQ102 defines no
  // stage 10 (E_QUEST_STAGE, the record's check first)
  REQUIRE(log.Count("E_QUEST_DENIED") >= 1);
  Leave(p);
}

TEST_CASE("A report's acting player is relayed as reported when it names a "
          "connected player, else stamped as the reporter (E_QUEST_ACTING)",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  QuestLog log;
  // read 7: the watcher's seat applied the reporter's stage and its
  // fragment's chained set ran under the reporter's scope
  ReportActing(p, 1, kMs13, 20, kReporter);
  REQUIRE(WordsTo(p, 0) == std::vector<Word>{ W(kMs13, 20, 0, kReporter) });
  REQUIRE(log.Count("E_QUEST_ACTING") == 0);
  // an actor no connected player holds
  p.Messages().clear();
  ReportActing(p, 1, kMs13, 10, 0xff000abc);
  REQUIRE(WordsTo(p, 0) == std::vector<Word>{ W(kMs13, 10, 0, kWatcher) });
  REQUIRE(log.Count("E_QUEST_ACTING") == 1);
  Leave(p);
}

TEST_CASE("Under questScope derived a player-class quest's stage and word are "
          "refused (E_QUEST_PLAYER), nothing recorded or relayed, and the "
          "login names it; under off it is the world's; a stage PQ-3 names "
          "is relayed served",
          "[QuestStage][espm]")
{
  constexpr uint32_t kWerewolf = 0x2ba16; // PlayerWerewolfQuest, by ALUA 0x7
  constexpr uint32_t kVampire = 0xeafd5;  // PlayerVampireQuest, by the list
  constexpr uint32_t kVc01 = 0x5c625;     // VC01, whose fragment cures
  {
    PartOne& p = GetPartOne();
    Enter(p, kDeniedQuests);
    // off (the default): measured, not acting, so the vampire's cure stage
    // is the world's like any other
    REQUIRE(p.PlayerClassQuests().empty());
    Report(p, 0, kVampire, 10);
    REQUIRE(p.GetQuestStages().size() == 1);
    REQUIRE(WordsTo(p, 1) ==
            std::vector<Word>{ W(kVampire, 10, 0, kReporter) });
    Leave(p);
  }
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);
  // VC01's first stage past 0, from its own record, as PQ-3's caller
  const auto vc01 =
    espm::Convert<espm::QUST>(p.GetEspm().GetBrowser().LookupById(kVc01).rec);
  REQUIRE(vc01);
  int cure = -1;
  for (const auto& stage : vc01->GetQuestStages(p.worldState.GetEspmCache())) {
    if (stage.journalIndex && stage.journalIndex->index > 0) {
      cure = stage.journalIndex->index;
      break;
    }
  }
  REQUIRE(cure > 0);
  p.SetPlayerQuestSettings(
    "{}", "{}", fmt::format(R"({{"5c625:Skyrim.esm": [{}]}})", cure));
  p.SetQuestScopeSettings(R"("derived")", R"(["eafd5:Skyrim.esm"])");
  QuestLog log;
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("2ba16:Skyrim.esm")) == 3);
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("eafd5:Skyrim.esm")) == 3);
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("4e50d:Skyrim.esm")) == 2);
  Report(p, 0, kWerewolf, 0);
  Report(p, 0, kVampire, 10);
  ReportWord(p, 0, kVampire, 1);
  REQUIRE(p.GetQuestStages().empty());
  REQUIRE(p.GetQuestRuns().empty());
  REQUIRE(WordsTo(p, 1).empty());
  // one line a minute per seat, the rest counted
  REQUIRE(log.Count("E_QUEST_PLAYER") == 1);

  // PQ-3: VC01's cure stage is recorded and relayed served
  Report(p, 0, kVc01, cure);
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{ W(kVc01, cure, 0, kReporter, true) });

  // the login names the player class after the denied quest, never
  // replaying its stages
  p.Messages().clear();
  p.SetUserActor(1, kWatcher);
  const auto sent = SentTo(p, 1);
  REQUIRE(!sent.empty());
  const auto& shared = std::get<2>(sent.front());
  const auto names = [&](uint32_t quest) {
    return std::find(shared.begin(), shared.end(), quest) != shared.end();
  };
  REQUIRE(names(kMq102));
  REQUIRE(names(kWerewolf));
  REQUIRE(names(kVampire));
  REQUIRE(!names(kVc01));
  // the ownership rule: the list names the class in `player` too, so each
  // seat runs its own player's quests' scripts
  nlohmann::json list;
  for (auto& m : p.Messages()) {
    if (m.userId == 1 &&
        m.j.value("t", -1) == static_cast<int>(MsgType::QuestStage) &&
        m.j.value("quest", 1u) == 0) {
      list = m.j;
    }
  }
  REQUIRE(list.contains("player"));
  const auto& player = list["player"];
  const auto inPlayer = [&](uint32_t quest) {
    return std::find(player.begin(), player.end(), quest) != player.end();
  };
  REQUIRE(inPlayer(kWerewolf));
  REQUIRE(inPlayer(kVampire));
  REQUIRE(!inPlayer(kMq102));
  REQUIRE(player.size() == p.PlayerClassQuests().size());
  Leave(p);
}

TEST_CASE("A seat flipping one quest's run past the churn bound holds it out "
          "of the record (E_QUEST_CHURN), its entry dropped and its later "
          "words refused",
          "[QuestStage][QuestRun][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);
  QuestLog log;
  // start, stop, start, ... one word a flip: six flips stay inside the
  // bound, the seventh holds the quest
  uint32_t word = 1;
  for (int i = 0; i < 7; ++i) {
    ReportWord(p, 0, kLetterQuest, word);
    word = 3 - word;
  }
  REQUIRE(p.GetQuestRuns().size() == 1);
  ReportWord(p, 0, kLetterQuest, word);
  REQUIRE(p.GetQuestRuns().empty());
  REQUIRE(log.Count("E_QUEST_CHURN") == 1);
  p.Messages().clear();
  ReportWord(p, 1, kLetterQuest, 1);
  ReportWord(p, 0, kLetterQuest, 2);
  REQUIRE(p.GetQuestRuns().empty());
  REQUIRE(WordsTo(p, 0).empty());
  REQUIRE(WordsTo(p, 1).empty());
  // no login replays it
  p.SetUserActor(1, kWatcher);
  REQUIRE(WordsTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("The login sends the list, the running quests' starts, the stages "
          "and the stopped quests' stops, all served; the old line byte for "
          "byte, the runs line beside it",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);
  ReportWord(p, 0, kLetterQuest, 1);
  ReportWord(p, 0, kHeartwoodMill, 1);
  ReportWord(p, 0, kHeartwoodMill, 2);
  Report(p, 0, kMs13, 10);
  QuestLog log;
  p.Messages().clear();
  p.SetUserActor(1, kWatcher);
  REQUIRE(SentTo(p, 1).front() ==
          Msg(0, 0, nlohmann::json::array({ kMq102 }), 1));
  // under off no class is named
  for (auto& m : p.Messages()) {
    REQUIRE(!m.j.contains("player"));
  }
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{ W(kLetterQuest, 0, 1, 0, true),
                             W(kMs13, 10, 0, 0, true),
                             W(kHeartwoodMill, 0, 2, 0, true) });
  REQUIRE(log.Count("QuestStage: actor ff000bd1 sent mode 1 with 1 denied "
                    "quests, 1 stages after a login") == 1);
  REQUIRE(log.Count("QuestStage: actor ff000bd1 sent 1 running and 1 stopped "
                    "quest(s) around the stages after a login") == 1);
  Leave(p);
}

// thuum ADR-068 rung 5 (D-Q3 A, the fresh review's note 7; Skyrim.esm,
// lab/esm.py on sky-srv, 2026-11-10): every start and stop word carries the
// stages whose fragments the engine runs with it, read from the quest's INDX
// flags as canon's QuestStageIsEngineStart read them. dunEldergleamT03
// defines stage 0 and flags 10 a start-up stage, no shut-down stage;
// ClearSkiesQuest flags 10 a start-up stage and 100 a shut-down one, and
// defines no stage 0; MS10MiscObjPointer flags neither and has no stage 0
namespace {
constexpr uint32_t kEldergleam = 0x15cc2;
constexpr uint32_t kClearSkies = 0x1e3b1;

// every run word `user` heard: its quest, its word and its `wordStages`
// (null when absent), in order
using Staged = std::tuple<uint32_t, uint32_t, nlohmann::json>;

std::vector<Staged> WordStagesTo(PartOne& p, Networking::UserId user)
{
  std::vector<Staged> sent;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::QuestStage) &&
        m.j.contains("word")) {
      sent.emplace_back(m.j.value("quest", 0u), m.j.value("word", 0u),
                        m.j.contains("wordStages") ? m.j["wordStages"]
                                                   : nlohmann::json());
    }
  }
  return sent;
}
}

TEST_CASE("Every start and stop word carries its engine stages, relayed and "
          "served: stage 0 and the start-up stages for a start, the "
          "shut-down stages for a stop, none when the record flags none",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);
  // ClearSkiesQuest is a quest a Story Manager quest node names (WeatherChange
  // 0x1e3c2, thuum docs/research/m3/re-story-location.md), whose runs D-Q4 C
  // keeps per seat under questStoryInstances "seat": this case reads the word
  // stages, so it runs under "world"
  p.SetQuestStoryInstancesSettings("\"world\"");
  QuestLog log;
  ReportWord(p, 0, kEldergleam, 1);
  ReportWord(p, 0, kClearSkies, 1);
  ReportWord(p, 0, kClearSkies, 2);
  ReportWord(p, 0, kLetterQuest, 1);
  using J = nlohmann::json;
  REQUIRE(WordStagesTo(p, 1) ==
          std::vector<Staged>{ { kEldergleam, 1, J::array({ 0, 10 }) },
                               { kClearSkies, 1, J::array({ 10 }) },
                               { kClearSkies, 2, J::array({ 100 }) },
                               { kLetterQuest, 1, J() } });
  REQUIRE(WordStagesTo(p, 0).empty());
  REQUIRE(log.Count("[verb:quest-stage] ff000bd0 starts 15cc2:Skyrim.esm: "
                    "recorded, relayed to 1 with its engine stages 0, "
                    "10") == 1);
  REQUIRE(log.Count("[verb:quest-stage] ff000bd0 starts 10d3bb:Skyrim.esm: "
                    "recorded, relayed to 1\n") == 1);

  // the login serves them the same way: the running quests' starts in the
  // run record's order, then the stopped quest's stop
  p.Messages().clear();
  p.SetUserActor(1, kWatcher);
  REQUIRE(WordStagesTo(p, 1) ==
          std::vector<Staged>{ { kEldergleam, 1, J::array({ 0, 10 }) },
                               { kLetterQuest, 1, J() },
                               { kClearSkies, 2, J::array({ 100 }) } });
  for (auto& m : p.Messages()) {
    if (m.j.contains("wordStages")) {
      REQUIRE(m.j.value("served", false));
    }
  }
  Leave(p);
}

// thuum ADR-068 rung 5, D-Q4 C (SCHEMA_VERSION 43): a seat's report names
// its cause, 1 a world cause, 2 its own, and its stack's root (R2). A run
// word, or a report the record judges a start (stage 0 or a start-up stage,
// read 5), whose cause is the seat's own is refused (E_QUEST_CAUSE), nothing
// written, relayed or counted; a stage crosses whatever its cause; a world
// cause is judged as every report was, and the lines name the cause and the
// root. dunEmbershardQST 0xb6fef, A Lovely Letter 0x74a83 and CR02 0x25185
// are Skyrim.esm's (the run-cause read of 2026-11-10, its census)
namespace {
constexpr uint32_t kEmbershard = 0xb6fef;
constexpr uint32_t kLovelyLetter = 0x74a83;
constexpr uint32_t kCr02 = 0x25185;

void ReportCaused(PartOne& p, Networking::UserId user, uint32_t quest,
                  int stage, uint32_t word, uint32_t cause, const char* root)
{
  nlohmann::json j{ { "t", static_cast<int>(MsgType::QuestStage) },
                    { "quest", quest },
                    { "stage", stage },
                    { "shared", nlohmann::json::array() },
                    { "mode", 0 },
                    { "cause", cause } };
  if (word != 0) {
    j["word"] = word;
  }
  if (root) {
    j["root"] = root;
  }
  DoMessage(p, user, j);
}
}

TEST_CASE("A run word or a start-up stage's report with the seat's own cause "
          "is refused (E_QUEST_CAUSE), nothing written or relayed; a stage "
          "crosses whatever its cause, and a world cause is recorded with "
          "its cause and root named",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);
  QuestLog log;
  // USSEP's housekeeping stop, rooted at its version tracker's OnInit, and
  // an engine start's start-up stage with no script call: the seat's own
  ReportCaused(p, 0, kLetterQuest, 0, 2, 2,
               "USKP_VersionTrackingScript.OnInit");
  ReportCaused(p, 0, kHeartwoodMill, 10, 0, 2, nullptr);
  ReportCaused(p, 0, kHeartwoodMill, 0, 1, 2, nullptr);
  REQUIRE(p.GetQuestRuns().empty());
  REQUIRE(p.GetQuestStages().empty());
  REQUIRE(WordsTo(p, 1).empty());
  REQUIRE(log.Count("E_QUEST_CAUSE: user 0 actor ff000bd0 reported the stop "
                    "of 10d3bb:Skyrim.esm (cause seat, root "
                    "USKP_VersionTrackingScript.OnInit, R2): the stop of "
                    "10d3bb:Skyrim.esm has the seat's own cause") == 1);
  // the start-up stage's report is the start's, the seat's own: no E_QUEST_
  // code (the designed path: the seat cannot tell a start-up stage), one
  // plain line a minute per seat; the same start sent as a word is refused
  // by its cause
  REQUIRE(log.Count("QuestStage: user 0 actor ff000bd0 reported stage 10 of "
                    "e3ea5:Skyrim.esm, which the record reads as its start "
                    "(cause seat, root none, R2): the seat's own") == 1);
  REQUIRE(log.Count("E_QUEST_CAUSE: user 0 actor ff000bd0 reported the start "
                    "of e3ea5:Skyrim.esm (cause seat, root none, R2)") == 1);
  REQUIRE(log.Count("E_QUEST_") == 2);
  REQUIRE(log.Count(": recorded, relayed") == 0);

  // HeartwoodMill's unflagged stage 20 with the seat's own cause and
  // Embershard's stage 40 with a world cause: stages, each recorded and
  // relayed, each line naming its cause
  ReportCaused(p, 0, kHeartwoodMill, 20, 0, 2,
               "FreeformHeartwoodMillScript.OnInit");
  ReportCaused(p, 1, kEmbershard, 40, 0, 1,
               "defaultSetStageTrigScript.OnTriggerEnter");
  const auto stages = p.GetQuestStages();
  REQUIRE(stages ==
          std::vector<QuestStageDone>{
            { FormDesc::FromString("e3ea5:Skyrim.esm"), 20 },
            { FormDesc::FromString("b6fef:Skyrim.esm"), 40 } });
  REQUIRE(log.Count("[verb:quest-stage] ff000bd0 set e3ea5:Skyrim.esm stage "
                    "20: recorded, relayed to 1 (cause seat, root "
                    "FreeformHeartwoodMillScript.OnInit, R2)") == 1);
  REQUIRE(log.Count("[verb:quest-stage] ff000bd1 set b6fef:Skyrim.esm stage "
                    "40: recorded, relayed to 1 (cause world, root "
                    "defaultSetStageTrigScript.OnTriggerEnter, R2)") == 1);

  // a world cause: the stop of the stage 40 fragment is recorded and
  // relayed, its line naming the root; the same word again folds
  p.Messages().clear();
  ReportCaused(p, 0, kEmbershard, 0, 2, 1,
               "QF_dunEmbershardQST_000B6FEF.Fragment_16");
  ReportCaused(p, 1, kEmbershard, 0, 2, 1,
               "QF_dunEmbershardQST_000B6FEF.Fragment_16");
  REQUIRE(p.GetQuestRuns() == Runs({ { "b6fef:Skyrim.esm", false, 1 } }));
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{ W(kEmbershard, 0, 2, kReporter) });
  REQUIRE(WordsTo(p, 0).empty());
  REQUIRE(log.Count("[verb:quest-stage] ff000bd0 stops b6fef:Skyrim.esm: "
                    "recorded, relayed to 1 (cause world, root "
                    "QF_dunEmbershardQST_000B6FEF.Fragment_16, R2)") == 1);
  // the refused words left the record alone: a world start of the letter
  // quest is recorded, not folded
  ReportCaused(p, 0, kLetterQuest, 0, 1, 1, "TIF__0010D3BC.Fragment_0");
  REQUIRE(p.GetQuestRuns() ==
          Runs({ { "b6fef:Skyrim.esm", false, 1 },
                 { "10d3bb:Skyrim.esm", true, 1 } }));
  Leave(p);
}

TEST_CASE("Under questStoryInstances seat a quest a Story Manager quest node "
          "names keeps its runs per seat (the designed path: a plain line "
          "naming the node, no E_QUEST_ code), its stages cross and the login "
          "serves none of its runs; under world it is judged by its cause",
          "[QuestStage][espm]")
{
  PartOne& p = GetPartOne();
  // the server's own read of the game files' SMQN nodes (thuum
  // docs/research/m3/re-story-location.md's Skyrim.esm dump:
  // FreeformRiverwoodNode 0xb555d's one NNAM 0x74a83, CompanionsAelaNode
  // 0xbfc77's three, CR02 0x25185 among them); dunEmbershardQST no node names
  REQUIRE(p.StoryNodesOf(FormDesc::FromString("74a83:Skyrim.esm")) ==
          "FreeformRiverwoodNode b555d");
  REQUIRE(p.StoryNodesOf(FormDesc::FromString("25185:Skyrim.esm")) ==
          "CompanionsAelaNode bfc77");
  REQUIRE(p.StoryNodesOf(FormDesc::FromString("b6fef:Skyrim.esm")).empty());
  REQUIRE_THROWS(p.SetQuestStoryInstancesSettings("\"off\""));

  Enter(p, kDeniedQuests);
  QuestLog log;
  // A Lovely Letter's start and stop with a world cause, and CR02's start
  // from an older client (no cause): each seat's own instance
  ReportCaused(p, 0, kLovelyLetter, 0, 1, 1,
               "QF_FreeformRiverwood01_00074A83.Fragment_22");
  ReportCaused(p, 0, kLovelyLetter, 0, 2, 1,
               "QF_FreeformRiverwood01_00074A83.Fragment_22");
  ReportWord(p, 1, kCr02, 1);
  REQUIRE(p.GetQuestRuns().empty());
  REQUIRE(WordsTo(p, 0).empty());
  REQUIRE(WordsTo(p, 1).empty());
  // the designed path (a world-caused completion's Stop comes here too):
  // one plain line a minute per seat and quest with every fact the refusal
  // had, and no E_QUEST_ code
  REQUIRE(log.Count("[verb:quest-stage] the start of 74a83:Skyrim.esm by "
                    "ff000bd0 kept to its seat: a Story Manager instance "
                    "(questStoryInstances seat; cause world, root "
                    "QF_FreeformRiverwood01_00074A83.Fragment_22, R2), named "
                    "by FreeformRiverwoodNode b555d, its stages still "
                    "crossing; user 0's report not recorded, nothing "
                    "relayed; 1 such report(s) of the quest since the last "
                    "line") == 1);
  REQUIRE(log.Count("the stop of 74a83:Skyrim.esm by ff000bd0 kept to its "
                    "seat") == 0); // counted, its line due a minute on
  REQUIRE(log.Count("[verb:quest-stage] the start of 25185:Skyrim.esm by "
                    "ff000bd1 kept to its seat: a Story Manager instance "
                    "(questStoryInstances seat), named by CompanionsAelaNode "
                    "bfc77, its stages still crossing; user 1's report not "
                    "recorded") == 1);
  REQUIRE(log.Count("E_QUEST_") == 0);
  // its stage crosses: A Lovely Letter's stage 10, recorded and relayed
  ReportCaused(p, 0, kLovelyLetter, 10, 0, 1,
               "QF_FreeformRiverwood01_00074A83.Fragment_3");
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{ W(kLovelyLetter, 10, 0, kReporter) });

  // a run recorded before (a world restarted from an older build): the login
  // serves none of an instance's runs under "seat"
  REQUIRE(
    p.SetQuestRuns({ { FormDesc::FromString("74a83:Skyrim.esm"), true, 1 },
                     { FormDesc::FromString("b6fef:Skyrim.esm"), true, 1 } }));
  p.Messages().clear();
  p.SetUserActor(1, kWatcher);
  size_t embershardStarts = 0;
  for (const auto& w : WordsTo(p, 1)) {
    // a word (its third member), never the instance's; its stage 10 is
    // served as any recorded stage is
    if (std::get<2>(w) != 0) {
      REQUIRE(std::get<0>(w) != kLovelyLetter);
      embershardStarts += std::get<0>(w) == kEmbershard ? 1 : 0;
    }
  }
  REQUIRE(embershardStarts == 1);
  REQUIRE(log.Count("QuestStage: actor ff000bd1 sent 1 running and 0 stopped "
                    "quest(s) around the stages after a login") == 1);

  // "world": the instance is judged by its cause like any other quest
  p.SetQuestStoryInstancesSettings("\"world\"");
  REQUIRE(p.SetQuestRuns({}));
  p.Messages().clear();
  ReportCaused(p, 0, kLovelyLetter, 0, 1, 1,
               "QF_FreeformRiverwood01_00074A83.Fragment_22");
  REQUIRE(p.GetQuestRuns() == Runs({ { "74a83:Skyrim.esm", true, 1 } }));
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{ W(kLovelyLetter, 0, 1, kReporter) });
  ReportCaused(p, 0, kLovelyLetter, 0, 2, 2,
               "FreeformRiverwood01Script.OnInit");
  REQUIRE(p.GetQuestRuns() == Runs({ { "74a83:Skyrim.esm", true, 1 } }));
  REQUIRE(log.Count("E_QUEST_CAUSE: user 0 actor ff000bd0 reported the stop "
                    "of 74a83:Skyrim.esm") == 1);
  // the run's one E_QUEST_ line is that genuine refusal
  REQUIRE(log.Count("E_QUEST_") == 1);
  Leave(p);
}

// thuum ADR-068 rung 5, D-Q3 A: a served replay's fragments are the world's
// history and never run again on a seat. A stage served for a quest not
// running on the seat makes the seat start the quest first, and that start
// runs its start-up stages' fragments unless a start word named them, so the
// login serves a quest recorded stopped whose stages it replays a start word
// ahead of them, stamped as a running quest's is, and its stop behind them
TEST_CASE("The login starts a quest recorded stopped ahead of the stages it "
          "replays, served with its start-up stages, and stops it behind "
          "them; a stopped quest with no stage and a Story Manager instance "
          "get no start, and a running quest's replay is as before",
          "[QuestStage][QuestRun][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kDeniedQuests);
  // Embershard running with its stage 40; HeartwoodMill started, its plain
  // stage 20 set, stopped; MS13 started, its stages 20 and 10 set, stopped;
  // the letter quest started and stopped with no stage
  ReportWord(p, 0, kEmbershard, 1);
  Report(p, 0, kEmbershard, 40);
  ReportWord(p, 0, kHeartwoodMill, 1);
  Report(p, 0, kHeartwoodMill, 20);
  ReportWord(p, 0, kHeartwoodMill, 2);
  ReportWord(p, 0, kMs13, 1);
  Report(p, 0, kMs13, 20);
  Report(p, 0, kMs13, 10);
  ReportWord(p, 0, kMs13, 2);
  ReportWord(p, 0, kLetterQuest, 1);
  ReportWord(p, 0, kLetterQuest, 2);
  // A Lovely Letter, a Story Manager instance under "seat": its stage 10
  // crosses, and a stopped run recorded before (an older build's) stays in
  // the record, unsent
  Report(p, 0, kLovelyLetter, 10);
  auto runs = p.GetQuestRuns();
  runs.push_back({ FormDesc::FromString("74a83:Skyrim.esm"), false, 2 });
  REQUIRE(p.SetQuestRuns(runs));
  // each word the login serves, as the watcher heard it relayed with its
  // engine stages: the running quest's start, the two stopped quests' starts
  // ahead of their stages, the three stops
  const auto relayed = WordStagesTo(p, 1);
  const std::vector<std::pair<uint32_t, uint32_t>> words{
    { kEmbershard, 1 },    { kHeartwoodMill, 1 }, { kMs13, 1 },
    { kHeartwoodMill, 2 }, { kMs13, 2 },          { kLetterQuest, 2 }
  };
  std::vector<Staged> served;
  for (const auto& [quest, word] : words) {
    const auto it =
      std::find_if(relayed.begin(), relayed.end(), [&](const Staged& s) {
        return std::get<0>(s) == quest && std::get<1>(s) == word;
      });
    REQUIRE(it != relayed.end());
    served.push_back(*it);
  }
  // HeartwoodMill's start names its start-up stage 10
  using J = nlohmann::json;
  REQUIRE(std::get<2>(served[1]) == J::array({ 10 }));

  QuestLog log;
  p.Messages().clear();
  p.SetUserActor(1, kWatcher);
  REQUIRE(WordsTo(p, 1) ==
          std::vector<Word>{
            W(kEmbershard, 0, 1, 0, true), W(kHeartwoodMill, 0, 1, 0, true),
            W(kMs13, 0, 1, 0, true), W(kEmbershard, 40, 0, 0, true),
            W(kHeartwoodMill, 20, 0, 0, true), W(kMs13, 10, 0, 0, true),
            W(kMs13, 20, 0, 0, true), W(kLovelyLetter, 10, 0, 0, true),
            W(kHeartwoodMill, 0, 2, 0, true), W(kMs13, 0, 2, 0, true),
            W(kLetterQuest, 0, 2, 0, true) });
  REQUIRE(WordStagesTo(p, 1) == served);
  REQUIRE(log.Count("QuestStage: actor ff000bd1 sent mode 1 with 1 denied "
                    "quests, 5 stages after a login") == 1);
  REQUIRE(log.Count("QuestStage: actor ff000bd1 sent 1 running and 3 stopped "
                    "quest(s) around the stages after a login, 2 of the "
                    "stopped started ahead of their stages (D-Q3 A)") == 1);
  Leave(p);
}

TEST_CASE("The world's quest stages survive a restart through the journal's "
          "record, and a login after it replays them",
          "[QuestStage][espm]")
{
  PartOne& running = GetPartOne();
  Enter(running);
  Report(running, 0, kMs13, 10);
  const std::vector<QuestStageDone> recorded = {
    { FormDesc::FromString("39645:Skyrim.esm"), 10 }
  };
  REQUIRE(running.GetQuestStages() == recorded);
  // thuum ADR-068 rung 5 (rule 6): the quest's run rides the same record
  ReportWord(running, 0, kMs13, 1);
  const auto runs = Runs({ { "39645:Skyrim.esm", true, 1 } });
  REQUIRE(running.GetQuestRuns() == runs);

  // the journal's record is a server-made reference's, at the fixed id, on
  // the XMarker base, so WorldState::LoadChangeForm recreates it from the
  // record as it does every reference above 0xff000000
  const MpChangeForm saved = running.WorldJournal().GetChangeForm();
  REQUIRE(saved.recType == MpChangeForm::REFR);
  REQUIRE(saved.formDesc ==
          FormDesc::FromFormId(PartOne::kWorldJournalRefId,
                               running.worldState.espmFiles));
  REQUIRE(saved.baseDesc == FormDesc::FromString("3b:Skyrim.esm"));
  REQUIRE(saved.worldOrCellDesc == FormDesc::Tamriel());
  REQUIRE(saved.questStages == recorded);
  REQUIRE(saved.questRuns == runs);
  Leave(running);

  // The restarted server: a fresh world reading the saved record, as
  // AttachSaveStorage does for each record at start
  PartOne& restarted = GetPartOne();
  restarted.worldState.LoadChangeForm(saved, restarted.CreateFormCallbacks());
  REQUIRE(restarted.GetQuestStages() == recorded);
  REQUIRE(restarted.GetQuestRuns() == runs);

  restarted.SetSharedQuestsSettings(kSharedQuests);
  DoConnect(restarted, 0);
  restarted.CreateActor(kReporter, { 0, 0, 0 }, 0, 0x3c);
  restarted.SetUserActor(0, kReporter);
  REQUIRE(SentTo(restarted, 0) ==
          std::vector<Sent>{ Msg(0, 0, nlohmann::json::array({ kMs13 })),
                             Msg(kMs13, 0), Msg(kMs13, 10) });
  REQUIRE(
    WordsTo(restarted, 0) ==
    std::vector<Word>{ W(kMs13, 0, 1, 0, true), W(kMs13, 10, 0, 0, true) });
  restarted.SetSharedQuestsSettings("[]");
  restarted.DestroyActor(kReporter);
  DoDisconnect(restarted, 0);
}
