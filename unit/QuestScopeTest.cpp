#include "TestUtils.hpp"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <vector>

PartOne& GetPartOne();

// thuum ADR-068 rung 5 (D-Q1's derivation whole; docs/research/
// rung5-quests-2026-11-09.md section 8(a), reads H-QS1 and H-QS2): every QUST
// of the load order is classed by wire-rules quest_scope over its aliases as
// libespm reads them (AliasCreatedTest's idiom: the game files through
// GetPartOne()'s loader, the five masters), the generated list half
// (`playerQuests`) naming the quests with no alias whose scripts' state
// natives all aim at the player. PlayerWerewolfQuest 0x2ba16 is the player's
// by its alias 0's ALUA 0x7 (no ALFR), PlayerVampireQuest 0xeafd5 by the
// list (it has no alias), Dawnguard's DLC1PlayerVampireQuest 0x020071d0 by
// its aliases (the census over the deployed files); the vampire castle's
// controller DLC1VCController 0x0201a428, MS13 0x39645, A Lovely Letter
// FreeformRiverwood01 0x74a83, the Aventus rumor DB01Misc 0x1ea4f and VC01
// 0x5c625 are the world's (each has a world alias, or none and no list
// entry). None of the deploy's denied or allowed quests the five masters
// hold is the player's: the lists below are thuum's
// lab/deploy/sky-srv/server-settings.json sharedQuestsMode, pinned on
// 2026-11-09 (44 denied, 4 allowed; the census's two marked denied quests
// are mods' controllers, Thaumaturgy.esp and MysticismMagic.esp, which no
// master holds).
namespace {
const char* const kDeployDenied[] = {
  "3372b:Skyrim.esm",          "d0593:Skyrim.esm",
  "4e50d:Skyrim.esm",          "2bf9c:Skyrim.esm",
  "2610a:Skyrim.esm",          "d0800:Skyrim.esm",
  "2610c:Skyrim.esm",          "1c5d9:Skyrim.esm",
  "19e53:Skyrim.esm",          "d517a:Skyrim.esm",
  "e2d29:Skyrim.esm",          "4b2d9:Skyrim.esm",
  "d19b9:Skyrim.esm",          "1f251:Skyrim.esm",
  "54d0a:Skyrim.esm",          "21556:Skyrim.esm",
  "64a14:Skyrim.esm",          "c99ef:Skyrim.esm",
  "1ea50:Skyrim.esm",          "1ea51:Skyrim.esm",
  "50f2e:Skyrim.esm",          "a7b33:Skyrim.esm",
  "5f596:Skyrim.esm",          "17f05:Skyrim.esm",
  "3907c:Skyrim.esm",          "649fc:Skyrim.esm",
  "95f4f:Skyrim.esm",          "e66fc:Skyrim.esm",
  "d1984:Skyrim.esm",          "954d9:Skyrim.esm",
  "352a:Dawnguard.esm",        "d06c:Dawnguard.esm",
  "2f65:Dawnguard.esm",        "1a415:Dawnguard.esm",
  "4e24:Dawnguard.esm",        "17f8e:Dragonborn.esm",
  "16e02:Dragonborn.esm",      "18b13:Dragonborn.esm",
  "18b15:Dragonborn.esm",      "305d:HearthFires.esm",
  "f4f8c0:MysticismMagic.esp", "809:Thaumaturgy.esp",
  "2725d0:LostGrimoire.esp",   "1e2c7:UndeadFX.esp"
};
const char* const kDeployAllowed[] = { "74a83:Skyrim.esm", "39645:Skyrim.esm",
                                       "b6031:Skyrim.esm",
                                       "3dd29:Skyrim.esm" };

bool Marked(PartOne& p, const char* quest)
{
  const auto desc = FormDesc::FromString(quest);
  const auto& marked = p.QuestScopeMarked();
  return std::any_of(marked.begin(), marked.end(), [&](const FormDesc& q) {
    return q.shortFormId == desc.shortFormId && q.file == desc.file;
  });
}

bool OnTheMasters(const std::string& quest)
{
  for (const char* master : { ":Skyrim.esm", ":Update.esm", ":Dawnguard.esm",
                              ":HearthFires.esm", ":Dragonborn.esm" }) {
    const std::string suffix(master);
    if (quest.size() > suffix.size() &&
        quest.compare(quest.size() - suffix.size(), suffix.size(), suffix) ==
          0) {
      return true;
    }
  }
  return false;
}

struct ScopeLog
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  ScopeLog()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("quest-scope-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~ScopeLog() { spdlog::set_default_logger(old); }
};
}

TEST_CASE("The quest scope classes the masters' quests: the werewolf by its "
          "alias, the vampire by the list, the world's quests the world's, "
          "no denied or allowed quest the player's",
          "[QuestScope][espm]")
{
  PartOne& p = GetPartOne();
  ScopeLog log;
  p.SetSharedQuestsSettings(
    R"({"mode":"all","denied":["4e50d:Skyrim.esm"],"allowed":["39645:Skyrim.esm"]})");
  p.SetQuestScopeSettings(R"("derived")", R"(["eafd5:Skyrim.esm"])");

  // the player's (H-QS1 by ALUA 0x7, H-QS2 by the list, Dawnguard's by its
  // aliases)
  REQUIRE(Marked(p, "2ba16:Skyrim.esm"));
  REQUIRE(Marked(p, "eafd5:Skyrim.esm"));
  REQUIRE(Marked(p, "71d0:Dawnguard.esm"));
  // the world's
  for (const char* world :
       { "1a428:Dawnguard.esm", "39645:Skyrim.esm", "74a83:Skyrim.esm",
         "1ea4f:Skyrim.esm", "5c625:Skyrim.esm" }) {
    INFO(world);
    REQUIRE_FALSE(Marked(p, world));
  }
  // the deploy's lists: none the masters hold is the player's
  for (const char* quest : kDeployDenied) {
    if (OnTheMasters(quest)) {
      INFO(quest);
      REQUIRE_FALSE(Marked(p, quest));
    }
  }
  for (const char* quest : kDeployAllowed) {
    INFO(quest);
    REQUIRE_FALSE(Marked(p, quest));
  }

  // acting: composed into the shared quests, the denied refusal first
  REQUIRE(p.PlayerClassQuests().size() == p.QuestScopeMarked().size());
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("2ba16:Skyrim.esm")) == 3);
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("eafd5:Skyrim.esm")) == 3);
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("4e50d:Skyrim.esm")) == 2);
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("39645:Skyrim.esm")) == 0);
  const auto listed = p.SharedQuestsListed();
  REQUIRE(listed.size() == 1 + p.PlayerClassQuests().size());

  // the measurement line names the class and its overlap with the lists
  const std::string line = log.oss.str();
  REQUIRE(line.find("[verb:quest-stage] quest scope (derived): " +
                    std::to_string(p.QuestScopeMarked().size()) +
                    " per player: ") != std::string::npos);
  REQUIRE(line.find("; 0 denied, 0 allowed") != std::string::npos);
  REQUIRE(line.find("2ba16:Skyrim.esm") != std::string::npos);
}

TEST_CASE("Under questScope off the class is measured and composed nowhere; "
          "an unknown mode or a malformed list stops at the setting",
          "[QuestScope][espm]")
{
  PartOne& p = GetPartOne();
  p.SetSharedQuestsSettings(R"({"mode":"all","denied":[]})");
  p.SetQuestScopeSettings(R"("off")", R"(["eafd5:Skyrim.esm"])");
  REQUIRE(Marked(p, "2ba16:Skyrim.esm"));
  REQUIRE(Marked(p, "eafd5:Skyrim.esm"));
  REQUIRE(p.PlayerClassQuests().empty());
  REQUIRE(p.CheckSharedQuest(FormDesc::FromString("2ba16:Skyrim.esm")) == 0);
  REQUIRE(p.SharedQuestsListed().empty());
  REQUIRE_THROWS(p.SetQuestScopeSettings(R"("aliases")", "[]"));
  REQUIRE_THROWS(p.SetQuestScopeSettings(R"("off")", R"(["eafd5"])"));
  REQUIRE_THROWS(
    p.SetPlayerQuestSettings(R"({"eafd5:Skyrim.esm": "x"})", "{}", "{}"));
  REQUIRE_THROWS(p.SetPlayerQuestSettings(
    "{}", R"({"eafd5:Skyrim.esm": ["NoSeparator"]})", "{}"));
  REQUIRE_THROWS(
    p.SetPlayerQuestSettings("{}", "{}", R"({"5c625:Skyrim.esm": [70000]})"));
}

TEST_CASE("The player class's globals join the player globals while the "
          "scope acts, so a seat keeps them and WorldGlobal refuses them",
          "[QuestScope][espm]")
{
  PartOne& p = GetPartOne();
  // the census's playerQuestGlobals for the vampire quest: PlayerIsVampire
  // 0xed06d and VampireFeedReady 0xf69cf (thuum r5-lab's emit)
  p.SetPlayerGlobalsSettings(R"(["38:Skyrim.esm"])");
  p.SetPlayerQuestSettings(
    R"({"eafd5:Skyrim.esm": ["ed06d:Skyrim.esm", "f69cf:Skyrim.esm"]})", "{}",
    "{}");
  const auto watching = [&](uint32_t global) {
    const auto watch = p.PlayerGlobalsWatch();
    return std::find(watch.begin(), watch.end(), global) != watch.end();
  };
  p.SetQuestScopeSettings(R"("off")", R"(["eafd5:Skyrim.esm"])");
  REQUIRE(watching(0x38));
  REQUIRE_FALSE(watching(0xed06d));
  p.SetQuestScopeSettings(R"("derived")", R"(["eafd5:Skyrim.esm"])");
  REQUIRE(watching(0x38));
  REQUIRE(watching(0xed06d));
  REQUIRE(watching(0xf69cf));
  REQUIRE(p.PlayerGlobalsWatch().size() == 3);
  // the setting rewritten keeps the class's
  p.SetPlayerGlobalsSettings(R"(["38:Skyrim.esm", "ed06d:Skyrim.esm"])");
  REQUIRE(p.PlayerGlobalsWatch().size() == 3);
  p.SetQuestScopeSettings(R"("off")", "[]");
  REQUIRE(p.PlayerGlobalsWatch().size() == 2);
}
