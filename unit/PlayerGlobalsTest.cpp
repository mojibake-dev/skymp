#include "MpChangeForms.h"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

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
