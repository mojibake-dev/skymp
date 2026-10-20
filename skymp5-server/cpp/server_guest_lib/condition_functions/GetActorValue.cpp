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
  // the server holds it as the maximum (the base with the race's abilities
  // and the running buffs) times the percentage it keeps. For every other
  // actor value it knows the player's recorded base (the client's
  // ActorValues report, 0 to 163; 0 for an actor with no record) and the
  // modifier it holds: the race's constant abilities (a Nord's 50
  // ResistFrost) and the effects it applied itself (MpActor::
  // GetEffectModifier). HYPOTHESIS (docs/verbs/spell-cast.md, Hailstone):
  // a modifier from any other source the engine applies by itself, a perk's
  // or an enchantment's, is not seen here.
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
  return base.value_or(0.f) + actor.GetEffectModifier(av);
}
