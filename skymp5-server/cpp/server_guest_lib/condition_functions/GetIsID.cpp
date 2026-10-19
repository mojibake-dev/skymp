#include "GetIsID.h"
#include "MpActor.h"

const char* ConditionFunctions::GetIsID::GetName() const
{
  return "GetIsID";
}

uint16_t ConditionFunctions::GetIsID::GetFunctionIndex() const
{
  return 72;
}

float ConditionFunctions::GetIsID::Execute(
  MpActor& actor, uint32_t parameter1, [[maybe_unused]] uint32_t parameter2,
  const ConditionEvaluatorContext&)
{
  return actor.GetBaseId() == parameter1 ? 1.f : 0.f;
}
