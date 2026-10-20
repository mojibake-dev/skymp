#include "EffectWasDualCast.h"
#include "MpActor.h"

const char* ConditionFunctions::EffectWasDualCast::GetName() const
{
  return "EffectWasDualCast";
}

uint16_t ConditionFunctions::EffectWasDualCast::GetFunctionIndex() const
{
  return 724;
}

float ConditionFunctions::EffectWasDualCast::Execute(
  MpActor& actor, [[maybe_unused]] uint32_t parameter1,
  [[maybe_unused]] uint32_t parameter2,
  const ConditionEvaluatorContext& context)
{
  (void)actor;
  // the flag travels with the hit's claim; a caller that knows nothing of
  // the cast (the damage formulas' callers) leaves it false, so a 724 rider
  // never holds there, as it never did
  return context.effectWasDualCast ? 1.f : 0.f;
}
