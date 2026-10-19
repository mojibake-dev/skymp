#include "IsHostileToActor.h"
#include "ConditionsEvaluator.h"
#include "MpActor.h"
#include "WorldState.h"

const char* ConditionFunctions::IsHostileToActor::GetName() const
{
  return "IsHostileToActor";
}

uint16_t ConditionFunctions::IsHostileToActor::GetFunctionIndex() const
{
  return 719;
}

float ConditionFunctions::IsHostileToActor::Execute(
  MpActor& actor, [[maybe_unused]] uint32_t parameter1,
  [[maybe_unused]] uint32_t parameter2,
  const ConditionEvaluatorContext& context)
{
  // only the magic-effect caller names the pair; for the others (a perk's
  // or a recipe's conditions) the evaluator's old answer for a function it
  // lacked, true, stands until they name theirs
  if (context.effectVictimFormId == 0 || context.effectCasterFormId == 0) {
    return 1.f;
  }
  const uint32_t self = actor.GetFormId();
  const uint32_t other = self == context.effectVictimFormId
    ? context.effectCasterFormId
    : context.effectVictimFormId;
  WorldState* worldState = actor.GetParent();
  if (!worldState || !worldState->fightBetween) {
    return 0.f;
  }
  return worldState->fightBetween(self, other) ? 1.f : 0.f;
}
