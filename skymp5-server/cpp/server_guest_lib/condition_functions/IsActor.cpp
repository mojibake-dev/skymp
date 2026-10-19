#include "IsActor.h"
#include "MpActor.h"

const char* ConditionFunctions::IsActor::GetName() const
{
  return "IsActor";
}

uint16_t ConditionFunctions::IsActor::GetFunctionIndex() const
{
  return 353;
}

float ConditionFunctions::IsActor::Execute(
  MpActor& actor, [[maybe_unused]] uint32_t parameter1,
  [[maybe_unused]] uint32_t parameter2, const ConditionEvaluatorContext&)
{
  (void)actor;
  // every actor the evaluator names here is an MpActor; the engine's
  // IsActor on a non-actor reference never reaches the server (HYPOTHESIS:
  // the reference run-on types the evaluator does not resolve return
  // false before this)
  return 1.f;
}
