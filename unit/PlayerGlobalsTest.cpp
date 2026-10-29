#include "ActionListener.h"
#include "ConsoleCommandMessage.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "OnEquipMessage.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include <catch2/catch_all.hpp>
#include <optional>
#include <simdjson.h>
#include <variant>

PartOne& GetPartOne();

// thuum docs/verbs/player-globals.md (ADR-029's A2): the globals a player's
// game wrote for its player live in its change form, named by FormDesc, and
// a record from before the field reads as none (rule 6's migration).

TEST_CASE("The change form keeps a player's globals, and older records read "
          "as none",
          "[PlayerGlobals]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  // the Craftlord's unspent points and the MCM's general XP multiplier
  changeForm.playerGlobals = {
    { FormDesc::FromString("b1c19:Apocalypse - Magic of Skyrim.esp"), 3.f },
    { FormDesc::FromString("43cef:Apocalypse - Magic of Skyrim.esp"), 1.5f }
  };

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).playerGlobals ==
          changeForm.playerGlobals);

  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("playerGlobals"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).playerGlobals);
}

// The handler and the login, on Skyrim.esm's own globals (the test's load
// order lacks Apocalypse): GameHour 0x38 listed, GameDaysPassed 0x39 not
// (thuum lab/esm.py).
namespace {
constexpr uint32_t kActor = 0xff000bcd;
constexpr uint32_t kGameHour = 0x38;
constexpr uint32_t kGameDaysPassed = 0x39;

MpActor& Player(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(kActor);
}

void Report(PartOne& p, uint32_t global, float value)
{
  DoMessage(
    p, 0,
    nlohmann::json{
      { "t", static_cast<int>(MsgType::PlayerGlobals) },
      { "watch", nlohmann::json::array() },
      { "values", { { { "global", global }, { "value", value } } } } });
}

void Leave(PartOne& p)
{
  p.SetPlayerGlobalsSettings("[]");
  p.SetSurvivalFoodSettings("{}");
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}

// The player eats or drinks the record, as its game reports the equip
void Eat(PartOne& p, uint32_t baseId)
{
  RawMessageData raw;
  raw.userId = 0;
  OnEquipMessage msg;
  msg.baseId = baseId;
  p.GetActionListener().OnEquip(raw, msg);
}

// The player types a console command
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

// the ConsoleOutput lines user 0 received since the last clear
std::vector<std::pair<std::string, bool>> ConsoleLines(PartOne& p)
{
  std::vector<std::pair<std::string, bool>> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::ConsoleOutput) {
      out.emplace_back(m.j["text"].get<std::string>(),
                       m.j["refused"].get<bool>());
    }
  }
  return out;
}

// the (global, value) pairs of the PlayerGlobals messages user 0 received
// since the last clear, each carrying the list to watch (the client takes
// the list from every message)
std::vector<std::pair<uint32_t, float>> Sent(PartOne& p,
                                             const nlohmann::json& watch)
{
  std::vector<std::pair<uint32_t, float>> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j.contains("t") &&
        m.j["t"] == MsgType::PlayerGlobals) {
      REQUIRE(m.j["watch"] == watch);
      for (auto& v : m.j["values"]) {
        out.emplace_back(v["global"].get<uint32_t>(), v["value"].get<float>());
      }
    }
  }
  return out;
}

// the recorded value of a global, none when the record lacks it
std::optional<float> Recorded(const MpActor& ac, const char* desc)
{
  for (const auto& g : ac.GetPlayerGlobals()) {
    if (g.global == FormDesc::FromString(desc)) {
      return g.value;
    }
  }
  return std::nullopt;
}
}

TEST_CASE("A listed global's report is recorded, an unlisted one refused, "
          "and the login sends the list and the record",
          "[PlayerGlobals][espm]")
{
  PartOne& p = GetPartOne();
  p.SetPlayerGlobalsSettings(R"(["38:Skyrim.esm"])");
  auto& ac = Player(p);

  Report(p, kGameHour, 7.f);
  const std::vector<PlayerGlobal> recorded = {
    { FormDesc::FromString("38:Skyrim.esm"), 7.f }
  };
  REQUIRE(ac.GetPlayerGlobals() == recorded);

  // not listed: the report is refused whole, the record unchanged
  Report(p, kGameDaysPassed, 3.f);
  REQUIRE(ac.GetPlayerGlobals() == recorded);

  // the login itself sends the list to watch and the record (train-30: at
  // SetUserActor, not at the first movement report, so the client holds the
  // values before its game's scripts run their first check); a second login
  // of the same actor is what a reconnect does
  p.Messages().clear();
  p.SetUserActor(0, kActor);
  bool sent = false;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j.contains("t") &&
        m.j["t"] == MsgType::PlayerGlobals) {
      REQUIRE(m.j["watch"] == nlohmann::json::array({ kGameHour }));
      REQUIRE(m.j["values"].size() == 1);
      REQUIRE(m.j["values"][0]["global"] == kGameHour);
      REQUIRE(m.j["values"][0]["value"] == 7.f);
      sent = true;
    }
  }
  REQUIRE(sent);
  Leave(p);
}

// thuum memo D51 B (2026-10-28, Eli: "that sounds good"): a listed global
// with a killAt kills the player from the record, the server's word; the
// test's load order lacks Survival, so GameHour stands in for the cold need
TEST_CASE("A listed global at or past its killAt kills the player by the "
          "server's word",
          "[PlayerGlobals]")
{
  auto& p = GetPartOne();
  p.SetPlayerGlobalsSettings(
    R"([{"global": "38:Skyrim.esm", "killAt": 1000}])");
  auto& ac = Player(p);
  REQUIRE_FALSE(ac.IsDead());
  Report(p, kGameHour, 999.f);
  REQUIRE_FALSE(ac.IsDead());
  Report(p, kGameHour, 1000.f);
  REQUIRE(ac.IsDead());
  // a dead player's further reports kill nothing twice
  Report(p, kGameHour, 1500.f);
  REQUIRE(ac.IsDead());
  Leave(p);
}

// thuum docs/verbs/player-globals.md, the needs (playtest 37, 2026-10-28,
// Eli: "food doesnt replenish hunger"): a food's listed effect lowers the
// recorded need by the table's amount, never below its floor, and the
// player's game is told the written value with the list to watch; a drink
// with no listed effect changes nothing; with the mode's switch off nothing
// moves. The test's load order lacks the Survival Creation, so the table is
// vanilla's: FoodRestoreHealth 0xf33cb, which FoodApple 0x64b2e carries and
// the healing potion 0x3eae3 (HealthRestorationTest's) does not (thuum
// lab/esm.py), GameHour 0x38 standing in for the hunger need and
// GameDaysPassed 0x39 for the mode's toggle, listed with the default 1
TEST_CASE("A food's listed effect lowers the recorded need by the table's "
          "amount, to its floor, and tells the game",
          "[PlayerGlobals][espm]")
{
  constexpr uint32_t kFoodApple = 0x64b2e;
  constexpr uint32_t kHealingPotion = 0x3eae3;
  PartOne& p = GetPartOne();
  p.SetPlayerGlobalsSettings(
    R"(["38:Skyrim.esm", {"global": "39:Skyrim.esm", "default": 1}])");
  p.SetSurvivalFoodSettings(
    R"({"switch": "39:Skyrim.esm", "effects": [{"effect": "f33cb:Skyrim.esm", "need": "38:Skyrim.esm", "amount": 18, "floor": 0}]})");
  auto& ac = Player(p);
  Report(p, kGameHour, 1000.f);
  const nlohmann::json watch = { kGameHour, kGameDaysPassed };

  // an apple: 1000 less the table's 18, the game told
  ac.AddItem(kFoodApple, 3);
  p.Messages().clear();
  Eat(p, kFoodApple);
  REQUIRE(Recorded(ac, "38:Skyrim.esm") == 982.f);
  REQUIRE(Sent(p, watch) ==
          std::vector<std::pair<uint32_t, float>>{ { kGameHour, 982.f } });

  // a drink with no listed effect: nothing recorded, nothing told
  ac.AddItem(kHealingPotion, 1);
  p.Messages().clear();
  Eat(p, kHealingPotion);
  REQUIRE(Recorded(ac, "38:Skyrim.esm") == 982.f);
  REQUIRE(Sent(p, watch).empty());

  // the mode's switch recorded off: nothing moves
  Report(p, kGameDaysPassed, 0.f);
  p.Messages().clear();
  Eat(p, kFoodApple);
  REQUIRE(Recorded(ac, "38:Skyrim.esm") == 982.f);
  REQUIRE(Sent(p, watch).empty());

  // the floor: 10 less 18 stops at 0
  Report(p, kGameDaysPassed, 1.f);
  Report(p, kGameHour, 10.f);
  p.Messages().clear();
  Eat(p, kFoodApple);
  REQUIRE(Recorded(ac, "38:Skyrim.esm") == 0.f);
  REQUIRE(Sent(p, watch) ==
          std::vector<std::pair<uint32_t, float>>{ { kGameHour, 0.f } });
  Leave(p);
}

// thuum docs/verbs/player-globals.md, the needs lever (Eli, 2026-10-13: a
// playtest's setup is server-side, through the console the lab's labCommand
// reaches, never typed in a game): SetGlobal writes one listed global into
// the target's record by the server's word and tells its game, by FormDesc
// or by form id, for the caller (0x14 or "self") or an actor; an unlisted
// global is refused and nothing is told; a moderator may not; a value at
// the settings' killAt kills, as a report's would (memo D51 B)
TEST_CASE("SetGlobal writes an admin's value into a listed player global, "
          "refuses an unlisted one and kills at a killAt",
          "[PlayerGlobals][espm]")
{
  PartOne& p = GetPartOne();
  const bool forAll = p.worldState.enableConsoleCommandsForAll;
  p.worldState.enableConsoleCommandsForAll = false;
  p.SetPlayerGlobalsSettings(R"(["38:Skyrim.esm"])");
  auto& ac = Player(p);
  Report(p, kGameHour, 1000.f);
  const nlohmann::json watch = { kGameHour };

  // a moderator may not
  ac.SetStaffRank(1);
  p.Messages().clear();
  Console(p, "setglobal",
          { int64_t{ 0x14 }, std::string("38:Skyrim.esm"), int64_t{ 100 } });
  p.Tick();
  REQUIRE(ConsoleLines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "Not enough permissions to use this command", true } });
  REQUIRE(Recorded(ac, "38:Skyrim.esm") == 1000.f);
  REQUIRE(Sent(p, watch).empty());

  // an admin's, by FormDesc: the record and the game
  ac.SetStaffRank(2);
  p.Messages().clear();
  Console(p, "SetGlobal",
          { int64_t{ 0x14 }, std::string("38:Skyrim.esm"), int64_t{ 100 } });
  p.Tick();
  REQUIRE(ConsoleLines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "SetGlobal done: 38:Skyrim.esm = 100 for ff000bcd", false } });
  REQUIRE(Recorded(ac, "38:Skyrim.esm") == 100.f);
  REQUIRE(Sent(p, watch) ==
          std::vector<std::pair<uint32_t, float>>{ { kGameHour, 100.f } });

  // by form id, for "self", with a fraction as the console types one
  p.Messages().clear();
  Console(p, "setglobal",
          { std::string("self"), int64_t{ kGameHour }, std::string("7.5") });
  p.Tick();
  REQUIRE(ConsoleLines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "setglobal done: 38:Skyrim.esm = 7.5 for ff000bcd", false } });
  REQUIRE(Recorded(ac, "38:Skyrim.esm") == 7.5f);

  // an unlisted global is refused, the record unchanged, nothing told
  p.Messages().clear();
  Console(p, "setglobal",
          { int64_t{ 0x14 }, std::string("39:Skyrim.esm"), int64_t{ 3 } });
  p.Tick();
  REQUIRE(ConsoleLines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "Failed: 39:Skyrim.esm is not a listed player global", true } });
  REQUIRE(ac.GetPlayerGlobals() ==
          std::vector<PlayerGlobal>{
            { FormDesc::FromString("38:Skyrim.esm"), 7.5f } });
  REQUIRE(Sent(p, watch).empty());

  // a value at the settings' killAt is the server's word, as a report's
  p.SetPlayerGlobalsSettings(
    R"([{"global": "38:Skyrim.esm", "killAt": 1000}])");
  REQUIRE_FALSE(ac.IsDead());
  Console(p, "setglobal",
          { int64_t{ 0x14 }, std::string("38:Skyrim.esm"), int64_t{ 1000 } });
  p.Tick();
  REQUIRE(ac.IsDead());

  p.worldState.enableConsoleCommandsForAll = forAll;
  Leave(p);
}
