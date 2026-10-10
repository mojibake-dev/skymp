#include "ActorValueRecord.h"
#include "MpActor.h"
#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include "WorldState.h"
#include "libespm/ALCH.h"
#include "libespm/espm.h"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

PartOne& GetPartOne();

namespace {
constexpr uint32_t kActor = 0xff000000;

// A player (user 0) with a recorded base health of 100, at `health`
MpActor& Drinker(PartOne& p, float health)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kActor);
  auto& actor = p.worldState.GetFormAt<MpActor>(kActor);
  ActorValueRecord record;
  record.bases = { { 24, 100.f } };
  REQUIRE(actor.SetActorValueRecord(record));
  ActorValues values = actor.GetActorValues();
  values.healthPercentage = health;
  actor.SetPercentages(values);
  return actor;
}

float Health(MpActor& actor)
{
  return actor.GetChangeForm().actorValues.healthPercentage;
}

void Drink(PartOne& p, MpActor& actor, uint32_t potion)
{
  const auto effects = espm::GetData<espm::ALCH>(potion, &p.worldState).effects;
  actor.ApplyEffects(potion, effects, 1.f, 0, false);
}

void Leave(PartOne& p)
{
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}
}

// Skyrim.esm (thuum lab/esm.py, 2026-10-10): RestoreHealth01 0x3EADD, a
// Potion of Minor Healing (AlchRestoreHealth 25, no duration);
// DB03Poison 0x58CFB (AlchDamageHealthDuration 6 for 10 s); FortifyHealth01
// 0x3EAF2 (AlchFortifyHealth, a peak value modifier with Recover, 20 for
// 60 s)
TEST_CASE("A Potion of Minor Healing heals 25 at once", "[MagicEffects][espm]")
{
  PartOne& p = GetPartOne();
  auto& actor = Drinker(p, 0.5f);
  Drink(p, actor, 0x3EADD);
  REQUIRE(Health(actor) == Catch::Approx(0.75f));
  REQUIRE(!actor.HasRunningEffects());
  Leave(p);
}

TEST_CASE("A lingering poison wounds each second of its duration",
          "[MagicEffects][espm]")
{
  PartOne& p = GetPartOne();
  auto& actor = Drinker(p, 1.f);
  Drink(p, actor, 0x58CFB);
  REQUIRE(Health(actor) == Catch::Approx(1.f));
  REQUIRE(actor.HasRunningEffects());
  actor.AdvanceEffects(4.f);
  REQUIRE(Health(actor) == Catch::Approx(0.76f));
  actor.AdvanceEffects(10.f);
  REQUIRE(Health(actor) == Catch::Approx(0.4f));
  REQUIRE(!actor.HasRunningEffects());
  Leave(p);
}

TEST_CASE("A Fortify Health raises the maximum while it runs and gives it "
          "back",
          "[MagicEffects][espm]")
{
  PartOne& p = GetPartOne();
  auto& actor = Drinker(p, 1.f);
  Drink(p, actor, 0x3EAF2);
  REQUIRE(actor.GetMaximumValues().health == Catch::Approx(120.f));
  REQUIRE(Health(actor) == Catch::Approx(1.f));
  // 30 points of damage while fortified: 90 of 120
  actor.DamageActorValue(espm::ActorValue::Health, 30.f);
  REQUIRE(Health(actor) == Catch::Approx(0.75f));
  actor.AdvanceEffects(61.f);
  REQUIRE(actor.GetMaximumValues().health == Catch::Approx(100.f));
  REQUIRE(Health(actor) == Catch::Approx(0.7f));
  REQUIRE(!actor.HasRunningEffects());
  Leave(p);
}

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
