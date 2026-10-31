#include "ActionListener.h"
#include "MsgType.h"
#include "PerkStateMessage.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum docs/verbs/perks.md (M5): perks as transactions. The rule itself is
// Rust's (cargo test -p wire-rules perks); these check the core's half: the
// table from the game files, the pick into the record with the state sent,
// the refusals, the switch, the natives' and the console's writes. The
// records as lab/esm.py read Skyrim.esm (2026-10-31): Armsman00 0xbabe4 has
// no condition; Armsman20 0x79343 wants GetBaseActorValue(OneHanded 6) >= 20
// and HasPerk(Armsman00); FightingStance 0x52d50 wants HasPerk(Armsman00)
// and One-handed 20.

namespace {
constexpr uint32_t kArmsman = 0xbabe4;
constexpr uint32_t kArmsman20 = 0x79343;
constexpr uint32_t kFightingStance = 0x52d50;
constexpr uint32_t kPicker = 0xff000fc1;
constexpr uint8_t kOneHandedIndex = 0; // PlayerSkills' index of One-handed

void Enter(PartOne& p, uint16_t level, float oneHanded)
{
  p.SetPerksEnabled(true);
  DoConnect(p, 0);
  p.CreateActor(kPicker, { 1.f, 2.f, 3.f }, 0, 0x3c);
  p.SetUserActor(0, kPicker);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  ActorValueRecord record;
  record.level = level;
  record.skills.push_back(
    ActorValueRecord::Skill{ kOneHandedIndex, oneHanded, 0.f, 0.f });
  picker.SetActorValueRecord(record);
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  p.DestroyActor(kPicker);
  p.SetPerksEnabled(false);
}

void Pick(PartOne& p, uint32_t perk)
{
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::PerkPick) },
                            { "perk", perk } });
}

std::vector<PerkStateMessage> States(PartOne& p)
{
  std::vector<PerkStateMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<PerkStateMessage*>(m.message.get());
    if (m.userId == 0 && typed) {
      REQUIRE(m.j["t"] == 68);
      REQUIRE(m.reliable);
      out.push_back(*typed);
    }
  }
  return out;
}
}

TEST_CASE("A pick with no point left is refused and the state says so; the "
          "points come with the level",
          "[Perks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 1, 15.f);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  REQUIRE(picker.PerkPoints() == 0);

  Pick(p, kArmsman);
  REQUIRE_FALSE(picker.HasPerk(kArmsman));
  auto states = States(p);
  REQUIRE(states.size() == 1);
  REQUIRE(states[0].perks.empty());
  REQUIRE(states[0].points == 0);

  // the level set by the server (the console's SetLevel writes the record
  // the same way): two points, the state resent
  p.Messages().clear();
  {
    auto record = *picker.GetActorValueRecord();
    record.level = 3;
    picker.SetActorValueRecord(record);
    picker.SendPerkState();
  }
  REQUIRE(picker.GetActorValueRecord()->level == 3);
  REQUIRE(picker.PerkPoints() == 2);
  states = States(p);
  REQUIRE(states.size() == 1);
  REQUIRE(states[0].points == 2);

  // the pick takes: the record, the state with one point left
  p.Messages().clear();
  Pick(p, kArmsman);
  REQUIRE(picker.HasPerk(kArmsman));
  REQUIRE(picker.PerkPoints() == 1);
  states = States(p);
  REQUIRE(states.size() == 1);
  REQUIRE(states[0].perks == std::vector<uint32_t>{ kArmsman });
  REQUIRE(states[0].points == 1);

  // held already: refused, nothing changes
  p.Messages().clear();
  Pick(p, kArmsman);
  REQUIRE(picker.PerkPoints() == 1);
  REQUIRE(States(p).size() == 1);

  // the record keeps the perk across a load
  MpChangeForm saved = picker.GetChangeForm();
  REQUIRE(saved.actorValueRecord.has_value());
  REQUIRE(saved.actorValueRecord->perks == std::vector<uint32_t>{ kArmsman });
  Leave(p);
}

TEST_CASE("The tree's lines and the skill's requirement hold: the second "
          "rank needs the first and One-handed 20",
          "[Perks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 5, 15.f);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);

  // Armsman 20 without Armsman: the prerequisite
  Pick(p, kArmsman20);
  REQUIRE_FALSE(picker.HasPerk(kArmsman20));
  // Armsman, then Armsman 20 under the skill's 20: the skill
  Pick(p, kArmsman);
  REQUIRE(picker.HasPerk(kArmsman));
  Pick(p, kArmsman20);
  REQUIRE_FALSE(picker.HasPerk(kArmsman20));
  Pick(p, kFightingStance);
  REQUIRE_FALSE(picker.HasPerk(kFightingStance));
  // the skill at 20: both open
  auto record = *picker.GetActorValueRecord();
  record.skills[0].level = 20.f;
  picker.SetActorValueRecord(record);
  Pick(p, kArmsman20);
  REQUIRE(picker.HasPerk(kArmsman20));
  Pick(p, kFightingStance);
  REQUIRE(picker.HasPerk(kFightingStance));
  REQUIRE(picker.PerkPoints() == 1);
  REQUIRE(picker.GetActorValueRecord()->perks ==
          std::vector<uint32_t>{ kFightingStance, kArmsman20, kArmsman });
  Leave(p);
}

TEST_CASE("The server's own writes need no rule, and the switch off drops "
          "picks",
          "[Perks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 1, 15.f);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);

  // the server's write at level 1 (the console's AddPerk and the native):
  // the record takes it with no rule, the points read 0, the state goes
  REQUIRE(picker.AddPerkByServer(kArmsman20));
  picker.SendPerkState();
  REQUIRE(picker.HasPerk(kArmsman20));
  REQUIRE(picker.PerkPoints() == 0);
  REQUIRE(States(p).size() == 1);
  REQUIRE(States(p)[0].perks == std::vector<uint32_t>{ kArmsman20 });
  REQUIRE(picker.RemovePerkByServer(kArmsman20));
  REQUIRE_FALSE(picker.HasPerk(kArmsman20));
  REQUIRE_FALSE(picker.RemovePerkByServer(kArmsman20));
  REQUIRE(picker.AddPerkByServer(kArmsman));
  REQUIRE_FALSE(picker.AddPerkByServer(kArmsman));
  REQUIRE(picker.RemovePerkByServer(kArmsman));

  // the switch off: a pick is dropped, no state answers
  p.SetPerksEnabled(false);
  p.Messages().clear();
  Pick(p, kArmsman);
  REQUIRE_FALSE(picker.HasPerk(kArmsman));
  REQUIRE(States(p).empty());
  Leave(p);
}
