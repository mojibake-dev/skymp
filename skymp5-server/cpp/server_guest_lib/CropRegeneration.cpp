#include "CropRegeneration.h"
#include "GetBaseActorValues.h"
#include "MathUtils.h"
#include "MpActor.h"
#include "MpChangeForms.h"

namespace {

BaseActorValues GetValues(MpActor* actor)
{
  uint32_t baseId = actor->GetBaseId();
  auto appearance = actor->GetAppearance();
  uint32_t raceId = appearance ? appearance->raceId : 0;
  auto worldState = actor->GetParent();
  return GetBaseActorValues(worldState, baseId, raceId,
                            actor->GetTemplateChain());
}

}

float CropRegeneration(float newAttributeValue, float secondsAfterLastRegen,
                       float attributeRate, float attributeRateMult,
                       float oldAttributeValue, bool hasActiveMagicEffects)
{
  spdlog::trace(
    "[crop]: args=(newAttributeValue={}, secondsAfterLastRegen={}, "
    "attributerate={}, attributeRateMult={}, oldAttributeValue={}, "
    "hasActiveMagicEffects={})",
    newAttributeValue, secondsAfterLastRegen, attributeRate, attributeRateMult,
    oldAttributeValue, hasActiveMagicEffects);

  float validRegenerationPercentage =
    MathUtils::PercentToFloat(attributeRate) *
    MathUtils::PercentToFloat(attributeRateMult) * secondsAfterLastRegen;

  spdlog::trace("[crop]: validRegenerationPercentage={}",
                validRegenerationPercentage);

  validRegenerationPercentage =
    validRegenerationPercentage < 0.0f ? 0.0f : validRegenerationPercentage;
  float validAttributePercentage =
    oldAttributeValue + validRegenerationPercentage;

  spdlog::trace("[crop]: validAttributePercentage={}",
                validAttributePercentage);

  validAttributePercentage =
    validAttributePercentage > 1.0f ? 1.0f : validAttributePercentage;
  constexpr float kMaxOldPercentage = 1.f;

  spdlog::trace("[crop]: comparing received attribute value and valid one: "
                "newAttributeValue={}, validAttributePercentage={}",
                newAttributeValue, validAttributePercentage);

  if (newAttributeValue > validAttributePercentage) {
    return validAttributePercentage;
  }
  if (newAttributeValue < 0.0f) {
    return 0.0f;
  }
  // if (hasActiveMagicEffects &&
  //    !MathUtils::IsNearlyEqual(oldAttributeValue, kMaxOldPercentage)) {
  //  return validAttributePercentage;
  // }
  return newAttributeValue;
}

// thuum docs/verbs/magic-effects.md: each rate and multiplier plus the
// running buffs on it (a Fortify Health Regeneration while it runs)
RegenRate GetHealthRegenRate(MpActor* actor)
{
  const BaseActorValues baseValues = GetValues(actor);
  const ActorValues& actorValues = actor->GetActorValues();
  return { std::max(baseValues.healRate, actorValues.healRate) +
             actor->GetEffectModifier(espm::ActorValue::HealRate),
           std::max(baseValues.healRateMult, actorValues.healRateMult) +
             actor->GetEffectModifier(
               espm::ActorValue::HealRateMult_or_CombatHealthRegenMultMod) };
}

RegenRate GetMagickaRegenRate(MpActor* actor)
{
  const BaseActorValues baseValues = GetValues(actor);
  const ActorValues& actorValues = actor->GetActorValues();
  return { std::max(baseValues.magickaRate, actorValues.magickaRate) +
             actor->GetEffectModifier(espm::ActorValue::MagickaRate),
           std::max(baseValues.magickaRateMult, actorValues.magickaRateMult) +
             actor->GetEffectModifier(
               espm::ActorValue::
                 MagickaRateMult_or_CombatHealthRegenMultPowerMod) };
}

RegenRate GetStaminaRegenRate(MpActor* actor)
{
  const BaseActorValues baseValues = GetValues(actor);
  const ActorValues& actorValues = actor->GetActorValues();
  return { (actor->IsBlockActive()
              ? actorValues.staminaRate
              : std::max(baseValues.staminaRate, actorValues.staminaRate)) +
             actor->GetEffectModifier(espm::ActorValue::StaminaRate),
           std::max(baseValues.staminaRateMult, actorValues.staminaRateMult) +
             actor->GetEffectModifier(espm::ActorValue::StaminaRateMult) };
}

float CropHealthRegeneration(float newAttributeValue,
                             float secondsAfterLastRegen, MpActor* actor)
{
  const RegenRate r = GetHealthRegenRate(actor);
  const ActorValues& actorValues = actor->GetActorValues();
  const bool hasActiveMagicEffects = !actor->GetActiveMagicEffects().Empty();
  return CropRegeneration(newAttributeValue, secondsAfterLastRegen, r.rate,
                          r.rateMult, actorValues.healthPercentage,
                          hasActiveMagicEffects);
}

float CropMagickaRegeneration(float newAttributeValue,
                              float secondsAfterLastRegen, MpActor* actor)
{
  const RegenRate r = GetMagickaRegenRate(actor);
  const ActorValues& actorValues = actor->GetActorValues();
  const bool hasActiveMagicEffects = !actor->GetActiveMagicEffects().Empty();
  return CropRegeneration(newAttributeValue, secondsAfterLastRegen, r.rate,
                          r.rateMult, actorValues.magickaPercentage,
                          hasActiveMagicEffects);
}

float CropStaminaRegeneration(float newAttributeValue,
                              float secondsAfterLastRegen, MpActor* actor)
{
  const RegenRate r = GetStaminaRegenRate(actor);
  const ActorValues& actorValues = actor->GetActorValues();
  const bool hasActiveMagicEffects = !actor->GetActiveMagicEffects().Empty();
  return CropRegeneration(newAttributeValue, secondsAfterLastRegen, r.rate,
                          r.rateMult, actorValues.staminaPercentage,
                          hasActiveMagicEffects);
}

float CropPeriodAfterLastRegen(float secondsAfterLastRegen,
                               float maxValidPeriod, float defaultPeriod)
{
  if (secondsAfterLastRegen < 0.0f) {
    return 0.0f;
  }
  if (secondsAfterLastRegen > maxValidPeriod) {
    return defaultPeriod;
  }
  return secondsAfterLastRegen;
}

float CropValue(float value, float min, float max)
{
  if (value < min) {
    return min;
  }
  if (value > max) {
    return max;
  }
  return value;
}
