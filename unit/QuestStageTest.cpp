#include "ActionListener.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <simdjson.h>
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
// the listed quest (its stage indexes include 5, 10 and 20), QUST 0xb6031
// MS13Intro a quest off the list, STAT 0x3b XMarker no quest at all.
namespace {
constexpr uint32_t kReporter = 0xff000bd0;
constexpr uint32_t kWatcher = 0xff000bd1;
constexpr uint32_t kMs13 = 0x39645;
constexpr uint32_t kMs13Intro = 0xb6031;
constexpr uint32_t kXMarker = 0x3b;
// a form of a sixth plugin, which the test's five-file load order lacks
constexpr uint32_t kUnloaded = 0x05000d62;
const char* const kSharedQuests = R"(["39645:Skyrim.esm"])";

// one QuestStage message as sent: the quest, the stage, the shared list
using Sent = std::tuple<uint32_t, int, nlohmann::json>;

Sent Msg(uint32_t quest, int stage,
         nlohmann::json shared = nlohmann::json::array())
{
  return Sent{ quest, stage, std::move(shared) };
}

void Enter(PartOne& p)
{
  p.SetSharedQuestsSettings(kSharedQuests);
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
                            { "shared", nlohmann::json::array() } });
}

std::vector<Sent> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<Sent> sent;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::QuestStage)) {
      sent.emplace_back(m.j.value("quest", 0u), m.j.value("stage", -1),
                        m.j.contains("shared") ? m.j["shared"]
                                               : nlohmann::json::array());
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
          "nothing; so is a client's report naming the list or quest 0",
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
                            { "shared", nlohmann::json::array({ kMs13 }) } });
  REQUIRE(p.GetQuestStages().empty());
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(SentTo(p, 0).empty());
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
  Leave(running);

  // The restarted server: a fresh world reading the saved record, as
  // AttachSaveStorage does for each record at start
  PartOne& restarted = GetPartOne();
  restarted.worldState.LoadChangeForm(saved, restarted.CreateFormCallbacks());
  REQUIRE(restarted.GetQuestStages() == recorded);

  restarted.SetSharedQuestsSettings(kSharedQuests);
  DoConnect(restarted, 0);
  restarted.CreateActor(kReporter, { 0, 0, 0 }, 0, 0x3c);
  restarted.SetUserActor(0, kReporter);
  REQUIRE(SentTo(restarted, 0) ==
          std::vector<Sent>{ Msg(0, 0, nlohmann::json::array({ kMs13 })),
                             Msg(kMs13, 10) });
  restarted.SetSharedQuestsSettings("[]");
  restarted.DestroyActor(kReporter);
  DoDisconnect(restarted, 0);
}
