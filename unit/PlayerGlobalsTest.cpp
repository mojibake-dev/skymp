#include "ActionListener.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

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
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
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
