#include "GetActorValue.h"
#include "MpActor.h"
#include "libespm/espm.h"

const char* ConditionFunctions::GetActorValue::GetName() const
{
  return "GetActorValue";
}

uint16_t ConditionFunctions::GetActorValue::GetFunctionIndex() const
{
  return 14;
}

float ConditionFunctions::GetActorValue::Execute(
  MpActor& actor, uint32_t parameter1, [[maybe_unused]] uint32_t parameter2,
  const ConditionEvaluatorContext&)
{
  // The engine answers the current value. For Health, Magicka and Stamina
  // the server holds it as the maximum (the base with the running buffs)
  // times the percentage it keeps. For every other actor value it knows
  // the player's recorded base (the client's ActorValues report, 0 to 163)
  // and the modifier of the effects it applied itself; an actor with no
  // record answers 0. HYPOTHESIS (docs/verbs/spell-cast.md, Hailstone): a
  // modifier the server never applied, a racial ability's frost
  // resistance above all, is not seen here, so a Nord reads 0 ResistFrost
  // until the race's abilities are read from the records.
  const auto av = static_cast<espm::ActorValue>(parameter1);
  if (av == espm::ActorValue::Health || av == espm::ActorValue::Magicka ||
      av == espm::ActorValue::Stamina) {
    const auto maximum = actor.GetMaximumValues();
    const auto& values = actor.GetActorValues();
    if (av == espm::ActorValue::Health) {
      return maximum.health * values.healthPercentage;
    }
    if (av == espm::ActorValue::Magicka) {
      return maximum.magicka * values.magickaPercentage;
    }
    return maximum.stamina * values.staminaPercentage;
  }
  if (parameter1 > 0xff) {
    return 0.f;
  }
  const auto base =
    actor.GetRecordedActorValueBase(static_cast<uint8_t>(parameter1));
  if (!base) {
    return 0.f;
  }
  return *base + actor.GetEffectModifier(av);
}
