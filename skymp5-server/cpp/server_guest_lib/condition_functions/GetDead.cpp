#include "GetDead.h"
#include "MpActor.h"

const char* ConditionFunctions::GetDead::GetName() const
{
  return "GetDead";
}

uint16_t ConditionFunctions::GetDead::GetFunctionIndex() const
{
  return 46;
}

float ConditionFunctions::GetDead::Execute(
  MpActor& actor, [[maybe_unused]] uint32_t parameter1,
  [[maybe_unused]] uint32_t parameter2, const ConditionEvaluatorContext&)
{
  (void)actor;
  // the server's own death record (thuum docs/verbs/death.md)
  return actor.IsDead() ? 1.f : 0.f;
}
