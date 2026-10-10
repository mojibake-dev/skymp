#include "MpChangeForms.h"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

// thuum docs/verbs/magic-effects.md: an actor's running effects outlast a
// restart in its change form, each with the seconds it has run; a caster is
// kept only when there is one
TEST_CASE("The change form keeps an actor's running effects, and older "
          "records read as none",
          "[MagicEffects]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;

  // DB03Poison's AlchDamageHealthDuration, 6 a second for 10 s, 3.5 s in,
  // from an attacker; FortifyHealth01's AlchFortifyHealth, 20 for 60 s,
  // drunk (Skyrim.esm, thuum lab/esm.py)
  RunningEffect poison;
  poison.effect = FormDesc::FromString("10aa4a:Skyrim.esm");
  poison.source = FormDesc::FromString("58cfb:Skyrim.esm");
  poison.caster = FormDesc::FromString("2");
  poison.kind = 0;
  poison.av = 24;
  poison.magnitude = 6.f;
  poison.durationS = 10.f;
  poison.elapsedS = 3.5f;
  poison.detrimental = true;

  RunningEffect fortify;
  fortify.effect = FormDesc::FromString("3eaf3:Skyrim.esm");
  fortify.source = FormDesc::FromString("3eaf2:Skyrim.esm");
  fortify.kind = 1;
  fortify.av = 24;
  fortify.magnitude = 20.f;
  fortify.durationS = 60.f;
  fortify.elapsedS = 12.25f;
  fortify.recover = true;

  changeForm.runningEffects = std::vector<RunningEffect>{ poison, fortify };

  simdjson::dom::parser parser;
  const nlohmann::json json = MpChangeForm::ToJson(changeForm);
  REQUIRE(!json["runningEffects"]["entries"][1].contains("caster"));
  const std::string dump = json.dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).runningEffects ==
          changeForm.runningEffects);

  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("runningEffects"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).runningEffects);
}
