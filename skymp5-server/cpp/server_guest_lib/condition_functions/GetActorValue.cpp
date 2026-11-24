#include "GetActorValue.h"
#include "MpActor.h"
#include "WorldState.h"
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
  if (parameter1 > 0xff) {
    return 0.f;
  }
  // Grabbed (CommonLibSSE-NG include/RE/A/ActorValues.h:169) is the engine's
  // mark on an actor another's grab holds, set only in the grabber's game;
  // the server's prop table holds that grab, so it answers 1 while the grab
  // holds the actor's body and 0 once it lets go
  if (parameter1 == static_cast<uint32_t>(espm::ActorValue::Grabbed)) {
    WorldState* worldState = actor.GetParent();
    if (worldState && worldState->propHeldOf) {
      return worldState->propHeldOf(actor.GetFormId()) ? 1.f : 0.f;
    }
  }
  return actor.GetCurrentActorValue(static_cast<espm::ActorValue>(parameter1));
}
