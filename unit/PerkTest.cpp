#include "ActionListener.h"
#include "ActorValuesMessage.h"
#include "ConsoleCommandMessage.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "PerkStateMessage.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

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
constexpr uint32_t kArmsman40 = 0x79342; // wants Armsman 20 and One-handed 40
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

// the seat's own actor values report, One-handed and the level as it says
// them (ActorValuesTest covers the report itself)
void Report(PartOne& p, uint16_t level, float oneHanded)
{
  ActorValuesMessage msg;
  ActorValuesMessage::Base base;
  base.av = 6; // One-handed (CommonLibSSE-NG ActorValues.h kOneHanded)
  base.base = oneHanded;
  msg.bases.push_back(base);
  ActorValuesMessage::Skill skill;
  skill.skill = kOneHandedIndex;
  skill.level = oneHanded;
  skill.threshold = 100.f; // the rule refuses a skill threshold of 0
  msg.skills.push_back(skill);
  msg.level = level;
  msg.xp = 0.f;
  msg.threshold = 100.f;
  RawMessageData raw;
  raw.userId = 0;
  p.GetActionListener().OnActorValues(raw, msg);
}

// a console command from the seat (ConsoleCommandTest's shape)
void Console(PartOne& p, const std::string& name,
             std::vector<std::variant<int64_t, std::string>> args)
{
  RawMessageData raw;
  raw.userId = 0;
  ConsoleCommandMessage msg;
  msg.data.commandName = name;
  msg.data.args = std::move(args);
  p.GetActionListener().OnConsoleCommand(raw, msg);
}

float RecordedOneHandedBase(const ActorValueRecord& record)
{
  for (const auto& [av, base] : record.bases) {
    if (av == 6) {
      return base;
    }
  }
  return -1.f;
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
  // and a server write sends no state either (the console's SetLevel would
  // otherwise strip a seat's own perks with an empty set)
  REQUIRE(picker.AddPerkByServer(kArmsman));
  picker.SendPerkState();
  REQUIRE(States(p).empty());
  REQUIRE(picker.RemovePerkByServer(kArmsman));
  Leave(p);
}

TEST_CASE("An accepted actor values report keeps the perks held",
          "[Perks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 3, 15.f);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  Pick(p, kArmsman);
  REQUIRE(picker.HasPerk(kArmsman));
  // the seat's report after the pick (its echo of the record, or a bar it
  // moved): the perks ride no report, so the merged record carries them
  Report(p, 3, 15.f);
  // the report was taken (its threshold landed), and the perks survived it
  REQUIRE(picker.GetActorValueRecord()->threshold == 100.f);
  REQUIRE(picker.HasPerk(kArmsman));
  REQUIRE(picker.GetActorValueRecord()->perks ==
          std::vector<uint32_t>{ kArmsman });
  REQUIRE(picker.PerkPoints() == 1);
  Leave(p);
}

TEST_CASE("With leveling on, a report moves no skill, experience or level, "
          "so a pick reads the server's numbers; with it off the seat's "
          "numbers stand",
          "[Perks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 5, 20.f);
  p.SetLevelingEnabled(true);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  Pick(p, kArmsman);
  Pick(p, kArmsman20);
  REQUIRE(picker.HasPerk(kArmsman20));
  REQUIRE(picker.PerkPoints() == 2);
  // a report claiming One-handed 100 at level 50: inside the wire's bounds,
  // outside the record (the server computes both under leveling)
  Report(p, 50, 100.f);
  {
    const auto record = *picker.GetActorValueRecord();
    REQUIRE(record.level == 5);
    REQUIRE(record.skills.at(0).level == 20.f);
    REQUIRE(RecordedOneHandedBase(record) == -1.f);
  }
  // Armsman 40 wants One-handed 40: the server's 20 refuses it
  Pick(p, kArmsman40);
  REQUIRE_FALSE(picker.HasPerk(kArmsman40));
  REQUIRE(picker.PerkPoints() == 2);
  // leveling off: the seat's numbers are the record's (R1, bounded), and
  // the same pick takes
  p.SetLevelingEnabled(false);
  Report(p, 50, 100.f);
  {
    const auto record = *picker.GetActorValueRecord();
    REQUIRE(record.level == 50);
    REQUIRE(record.skills.at(0).level == 100.f);
    REQUIRE(RecordedOneHandedBase(record) == 100.f);
    REQUIRE(record.perks == std::vector<uint32_t>{ kArmsman20, kArmsman });
  }
  Pick(p, kArmsman40);
  REQUIRE(picker.HasPerk(kArmsman40));
  Leave(p);
}

TEST_CASE("A pick before the actor values are recorded is refused with the "
          "state sent",
          "[Perks][espm]")
{
  PartOne& p = GetPartOne();
  p.SetPerksEnabled(true);
  DoConnect(p, 0);
  p.CreateActor(kPicker, { 1.f, 2.f, 3.f }, 0, 0x3c);
  p.SetUserActor(0, kPicker);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  p.Messages().clear();
  Pick(p, kArmsman);
  REQUIRE_FALSE(picker.HasPerk(kArmsman));
  auto states = States(p);
  REQUIRE(states.size() == 1);
  REQUIRE(states[0].perks.empty());
  REQUIRE(states[0].points == 0);
  Leave(p);
}

TEST_CASE("The console's SetLevel moves the points and sends the state",
          "[Perks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 1, 15.f);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  picker.SetConsoleCommandsAllowedFlag(true); // the console is an admin's
  Console(p, "SetLevel", { int64_t(0x14), int64_t(5) });
  REQUIRE(picker.GetActorValueRecord()->level == 5);
  REQUIRE(picker.PerkPoints() == 4);
  auto states = States(p);
  REQUIRE(states.size() == 1);
  REQUIRE(states[0].points == 4);
  Leave(p);
}

// playtest 41's item 6 (2026-11-02): a perk the console, a quest or a mod
// granted counted against the points, so a seat set up with AddPerk had no
// pick at its level-up. The points are a counter now: one a level past the
// first, less the picks the rule accepted; a server-side add spends nothing.
TEST_CASE("A perk granted outside a pick spends no point; the pick does",
          "[Perks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 2, 25.f);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  REQUIRE(picker.PerkPoints() == 1);

  // the console's or a gamemode's add: held, the point untouched
  REQUIRE(picker.AddPerkByServer(kArmsman));
  REQUIRE(picker.HasPerk(kArmsman));
  REQUIRE(picker.PerkPoints() == 1);
  REQUIRE(picker.GetActorValueRecord()->perkPicks == 0);

  // the pick of the perk that wants Armsman takes and spends the point
  p.Messages().clear();
  Pick(p, kFightingStance);
  REQUIRE(picker.HasPerk(kFightingStance));
  REQUIRE(picker.GetActorValueRecord()->perkPicks == 1);
  REQUIRE(picker.PerkPoints() == 0);
  auto states = States(p);
  REQUIRE(states.size() == 1);
  REQUIRE(states[0].points == 0);

  // no point left: refused, the two perks held stay
  p.Messages().clear();
  Pick(p, kArmsman20);
  REQUIRE_FALSE(picker.HasPerk(kArmsman20));
  REQUIRE(picker.GetActorValueRecord()->perkPicks == 1);
  REQUIRE(States(p).size() == 1);

  // the counter rides the record across a load
  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(picker.GetChangeForm()).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(
    MpChangeForm::JsonToChangeForm(element).actorValueRecord->perkPicks == 1);
  Leave(p);
}

TEST_CASE("A record from before the picks counter reads its picks as the "
          "perks held, capped at the points its level earned",
          "[Perks]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  ActorValueRecord record;
  record.level = 5;
  record.perks = { kArmsman, kFightingStance };
  record.perkPicks = 7; // written, then erased below as an older save lacks it
  changeForm.actorValueRecord = record;

  simdjson::dom::parser parser;
  auto older = MpChangeForm::ToJson(changeForm);
  REQUIRE(older["actorValueRecord"].erase("perkPicks") == 1);
  const std::string dump = older.dump();
  auto element = parser.parse(dump).value();
  // level 5 earned 4 points; two perks held read as two picks
  REQUIRE(
    MpChangeForm::JsonToChangeForm(element).actorValueRecord->perkPicks == 2);

  // more perks than points (a seat set up with AddPerk): the cap, so the
  // record shows no negative count and no free point either
  changeForm.actorValueRecord->level = 2;
  changeForm.actorValueRecord->perks = { kArmsman, kArmsman20,
                                         kFightingStance };
  older = MpChangeForm::ToJson(changeForm);
  REQUIRE(older["actorValueRecord"].erase("perkPicks") == 1);
  const std::string capped = older.dump();
  auto cappedElement = parser.parse(capped).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(cappedElement)
            .actorValueRecord->perkPicks == 1);

  // a record carrying the counter reads it as written
  const std::string current = MpChangeForm::ToJson(changeForm).dump();
  auto currentElement = parser.parse(current).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(currentElement)
            .actorValueRecord->perkPicks == 7);
}
