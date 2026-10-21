#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActorValueRecord.h"
#include "MpActor.h"
#include "libespm/espm.h"

PartOne& GetPartOne();

// thuum docs/verbs/magic-effects.md (train-30, resistance): the server
// imposes another actor's Health, Magicka and Stamina damage on a player
// (R0) and the engine's own resistance never runs on it, so the server
// takes the target's share: Firebolt's 25 (Skyrim.esm 0x12FD0, fire)
// against Resist Fire and Resist Magic on the player's record, each a
// percentage off, the element after the magic, capped at 85; a buff that
// is not hostile is untouched.
namespace {
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kFortifyHealthPotion = 0x0003eaf2;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;
constexpr uint8_t kResistFire = 41;  // CommonLibSSE-NG ActorValues.h
constexpr uint8_t kResistMagic = 44; // CommonLibSSE-NG ActorValues.h

std::pair<MpActor*, MpActor*> Pair(PartOne& p)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kCaster, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kCaster);
  p.CreateActor(kTarget, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  return { &p.worldState.GetFormAt<MpActor>(kCaster),
           &p.worldState.GetFormAt<MpActor>(kTarget) };
}

void Leave(PartOne& p)
{
  p.DestroyActor(kCaster);
  p.DestroyActor(kTarget);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}

float Health(MpActor& actor)
{
  return actor.GetActorValues().healthPercentage;
}

void Hit(PartOne& p, MpActor& target, float resistFire, float resistMagic)
{
  ActorValueRecord record;
  record.bases = { { kResistFire, resistFire },
                   { kResistMagic, resistMagic } };
  target.SetActorValueRecord(record);
  MpChangeForm changeForm = target.GetChangeForm();
  changeForm.actorValues.healthPercentage = 1.f;
  target.ApplyChangeForm(changeForm);
  target.ApplyEffects(kFirebolt, MpActor::EffectsOf(&p.worldState, kFirebolt),
                      1.f, kCaster, false);
}
}

TEST_CASE("A player's resistances take their share of a hostile effect: "
          "Firebolt's 25 against Resist Fire and Resist Magic, under the cap",
          "[Resistance][espm]")
{
  PartOne& p = GetPartOne();
  auto [caster, target] = Pair(p);
  Hit(p, *target, 0.f, 0.f);
  REQUIRE(Health(*target) == Catch::Approx(0.75f));
  Hit(p, *target, 50.f, 0.f);
  REQUIRE(Health(*target) == Catch::Approx(0.875f));
  Hit(p, *target, 50.f, 50.f);
  REQUIRE(Health(*target) == Catch::Approx(0.9375f));
  // the player's cap: 100 holds at 85
  Hit(p, *target, 100.f, 0.f);
  REQUIRE(Health(*target) == Catch::Approx(0.9625f));
  // a weakness adds
  Hit(p, *target, -100.f, 0.f);
  REQUIRE(Health(*target) == Catch::Approx(0.5f));
  Leave(p);
}

TEST_CASE("A buff that is not hostile is not resisted: Fortify Health under "
          "Resist Magic still moves the maximum by its magnitude",
          "[Resistance][espm]")
{
  PartOne& p = GetPartOne();
  auto [caster, target] = Pair(p);
  ActorValueRecord record;
  record.bases = { { kResistMagic, 50.f } };
  target->SetActorValueRecord(record);
  const float before = target->GetMaximumValues().health;
  target->ApplyEffects(kFortifyHealthPotion,
                       MpActor::EffectsOf(&p.worldState, kFortifyHealthPotion),
                       1.f, 0, false);
  REQUIRE(target->GetMaximumValues().health == Catch::Approx(before + 20.f));
  Leave(p);
}
