#include "ActionListener.h"
#include "ActorValuesMessage.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <cmath>

PartOne& GetPartOne();

// thuum docs/verbs/leveling.md (M5 Progression): a player's skill uses run
// the game's formulas on the server. The constants come from Skyrim.esm's
// AVIF records (lab/esm.py, 2026-10-31: One-handed 6.3, 0, 2, 0; Smithing
// 160, 0, 0.25, 300), the step from level 15 costs 2 * 15^1.95 (about 392.6)
// for One-handed, a use of a base of 10 earns 63, so the seventh hit takes
// the skill to 16 and the character 16 toward its first 100.

namespace {
constexpr uint32_t kActor = 0xff000fd1;
constexpr uint32_t kActorNoRecord = 0xff000fd2;
constexpr uint32_t kTamriel = 0x0000003c;
constexpr uint8_t kOneHanded = 6;
constexpr uint8_t kSmithing = 10;

MpActor& Player(PartOne& p, uint32_t actorId)
{
  DoConnect(p, 0);
  p.CreateActor(actorId, { 0, 0, 0 }, 0, kTamriel);
  p.SetUserActor(0, actorId);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(actorId);
}

// the player's first actor values report: One-handed at 15 with no progress
void FirstReport(PartOne& p)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::ActorValues) },
                    { "bases",
                      nlohmann::json::array(
                        { { { "av", kOneHanded }, { "base", 15.f } } }) },
                    { "skills",
                      nlohmann::json::array({ { { "skill", 0 },
                                                { "level", 15.f },
                                                { "xp", 0.f },
                                                { "threshold", 392.6f } } }) },
                    { "xp", 0.f },
                    { "threshold", 100.f },
                    { "level", 1 },
                    { "legendary", nlohmann::json::array() } });
}

void Use(PartOne& p, uint8_t skill, float amount)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SkillUse) },
                    { "uses",
                      nlohmann::json::array(
                        { { { "skill", skill }, { "amount", amount } } }) } });
}

std::vector<ActorValuesMessage> RecordsTo(PartOne& p, Networking::UserId user)
{
  std::vector<ActorValuesMessage> out;
  for (auto& m : p.Messages()) {
    auto values = dynamic_cast<ActorValuesMessage*>(m.message.get());
    if (m.userId == user && values) {
      out.push_back(*values);
    }
  }
  return out;
}

void Leave(PartOne& p, uint32_t actorId)
{
  p.SetLevelingEnabled(false);
  DoDisconnect(p, 0);
  p.DestroyActor(actorId);
}
}

TEST_CASE("The skill table is read from the game files at start",
          "[Leveling][espm]")
{
  PartOne& p = GetPartOne();
  REQUIRE(p.SkillUseXp(kOneHanded, 10.f) == Catch::Approx(63.f));
  REQUIRE(p.SkillUseXp(kSmithing, 1.f) == Catch::Approx(160.f));
  REQUIRE(p.SkillLevelCost(kOneHanded, 15) ==
          Catch::Approx(2.f * std::pow(15.f, 1.95f)).margin(0.01));
  REQUIRE(p.SkillLevelCost(kSmithing, 15) ==
          Catch::Approx(0.25f * std::pow(15.f, 1.95f) + 300.f).margin(0.01));
  // no skill: nothing
  REQUIRE(p.SkillUseXp(5, 10.f) == 0.f);
}

TEST_CASE("Seven One-handed uses of a base of 10 take the record from 15 to "
          "16, the base with it, and the record goes to the player's game; "
          "with leveling off a use changes nothing",
          "[Leveling][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActor);
  FirstReport(p);
  REQUIRE(ac.GetActorValueRecord().has_value());
  p.SetLevelingEnabled(true);
  p.Messages().clear();

  for (int hit = 1; hit <= 6; ++hit) {
    Use(p, kOneHanded, 10.f);
  }
  auto record = ac.GetActorValueRecord();
  REQUIRE(record.has_value());
  REQUIRE(record->skills.size() == 1);
  REQUIRE(record->skills[0].level == 15.f);
  REQUIRE(record->skills[0].xp == Catch::Approx(378.f));
  REQUIRE(record->level == 1);
  // no level moved yet: nothing sent
  REQUIRE(RecordsTo(p, 0).empty());

  Use(p, kOneHanded, 10.f);
  record = ac.GetActorValueRecord();
  REQUIRE(record->skills[0].level == 16.f);
  REQUIRE(record->skills[0].xp ==
          Catch::Approx(441.f - 2.f * std::pow(15.f, 1.95f)).margin(0.01));
  REQUIRE(record->skills[0].threshold ==
          Catch::Approx(2.f * std::pow(16.f, 1.95f)).margin(0.01));
  REQUIRE(record->bases.size() == 1);
  REQUIRE(record->bases[0].first == kOneHanded);
  REQUIRE(record->bases[0].second == 16.f);
  REQUIRE(record->level == 1);
  REQUIRE(record->xp == Catch::Approx(16.f));
  REQUIRE(record->threshold == Catch::Approx(100.f));
  const auto sent = RecordsTo(p, 0);
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].skills.size() == 1);
  REQUIRE(sent[0].skills[0].level == 16.f);
  REQUIRE(sent[0].bases.size() == 1);
  REQUIRE(sent[0].bases[0].base == 16.f);

  // a refused use (no skill at ActorValue 5) changes nothing
  p.Messages().clear();
  Use(p, 5, 10.f);
  REQUIRE(ac.GetActorValueRecord() == record);

  // the switch off: dropped whole
  p.SetLevelingEnabled(false);
  Use(p, kOneHanded, 10.f);
  REQUIRE(ac.GetActorValueRecord() == record);
  REQUIRE(RecordsTo(p, 0).empty());
  Leave(p, kActor);
}

TEST_CASE("A use before the player's first actor values report is dropped",
          "[Leveling][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActorNoRecord);
  p.SetLevelingEnabled(true);
  Use(p, kOneHanded, 10.f);
  REQUIRE(!ac.GetActorValueRecord().has_value());
  REQUIRE(RecordsTo(p, 0).empty());
  Leave(p, kActorNoRecord);
}
