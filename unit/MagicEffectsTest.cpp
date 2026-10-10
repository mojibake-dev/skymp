#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "MpActor.h"
#include "MpChangeForms.h"
#include "OnEquipMessage.h"
#include "TestUtils.hpp"
#include "WorldState.h"
#include "libespm/ALCH.h"
#include "libespm/espm.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <optional>
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

// The actor's own game hears its effects (every actor listens to itself):
// a set with the fortify and its 60 s, then an empty set at its end
TEST_CASE("The games that see an actor hear its running effects",
          "[MagicEffects][espm]")
{
  PartOne& p = GetPartOne();
  auto& actor = Drinker(p, 1.f);
  auto last = [&]() -> std::optional<nlohmann::json> {
    std::optional<nlohmann::json> found;
    for (auto& m : p.Messages()) {
      if (m.userId == 0 && m.j.contains("t") && m.j["t"] == 43) {
        REQUIRE(m.reliable);
        found = m.j;
      }
    }
    return found;
  };

  p.Messages().clear();
  Drink(p, actor, 0x3EAF2);
  auto set = last();
  REQUIRE(set);
  REQUIRE((*set)["effects"].size() == 1);
  REQUIRE((*set)["effects"][0]["effect"] == 0x3EAF3);
  REQUIRE((*set)["effects"][0]["remaining"].get<float>() ==
          Catch::Approx(60.f));

  p.Messages().clear();
  actor.AdvanceEffects(61.f);
  set = last();
  REQUIRE(set);
  REQUIRE((*set)["effects"].empty());
  Leave(p);
}

// DamageHealth01 0x3A5A4, a Weak Poison (AlchDamageHealth 15, ENIT poison):
// the legacy path drank it and restored 15
TEST_CASE("A poison is not drunk", "[MagicEffects][espm]")
{
  PartOne& p = GetPartOne();
  auto& actor = Drinker(p, 0.5f);
  actor.AddItem(0x3A5A4, 1);
  RawMessageData raw;
  raw.userId = 0;
  OnEquipMessage msg;
  msg.baseId = 0x3A5A4;
  p.GetActionListener().OnEquip(raw, msg);
  REQUIRE(Health(actor) == Catch::Approx(0.5f));
  REQUIRE(!actor.HasRunningEffects());
  Leave(p);
}

// An older record's legacy effect (FortifyHealRate01's AlchFortifyHealRate
// 0x3EB06, a peak value modifier with Recover on the health regeneration
// multiplier, actor value 155: 50 for 300 s, 100 s left) and the
// multiplier the legacy path set (base times mult times 4, a patch)
TEST_CASE("An older record's legacy effect becomes a running one with the "
          "time it has left",
          "[MagicEffects][espm]")
{
  PartOne& p = GetPartOne();
  auto& actor = Drinker(p, 1.f);
  MpChangeForm changeForm = actor.GetChangeForm();
  ActiveMagicEffectsMap::Entry legacy;
  legacy.data.effectId = 0x3EB06;
  legacy.data.magnitude = 50.f;
  legacy.data.duration = 300;
  legacy.endTime =
    std::chrono::system_clock::now() + std::chrono::seconds(100);
  changeForm.activeMagicEffects.Add(static_cast<espm::ActorValue>(155),
                                    legacy);
  changeForm.actorValues.healRateMult = 600.f;
  actor.ApplyChangeForm(changeForm);

  REQUIRE(actor.GetChangeForm().activeMagicEffects.Empty());
  REQUIRE(actor.HasRunningEffects());
  const auto& running = *actor.GetChangeForm().runningEffects;
  REQUIRE(running.size() == 1);
  REQUIRE(running[0].effect == FormDesc::FromString("3eb06:Skyrim.esm"));
  REQUIRE(running[0].elapsedS == Catch::Approx(200.f).margin(5.f));
  REQUIRE(actor.GetEffectModifier(
            espm::ActorValue::HealRateMult_or_CombatHealthRegenMultMod) ==
          Catch::Approx(50.f));
  REQUIRE(actor.GetChangeForm().actorValues.healRateMult < 600.f);
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
