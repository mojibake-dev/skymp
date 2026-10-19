#include "IsUndead.h"
#include "HasKeyword.h"
#include "MpActor.h"

namespace {
// Skyrim.esm's ActorTypeUndead (thuum lab/esm.py, 2026-10-18)
constexpr uint32_t kActorTypeUndead = 0x00013796;
}

const char* ConditionFunctions::IsUndead::GetName() const
{
  return "IsUndead";
}

uint16_t ConditionFunctions::IsUndead::GetFunctionIndex() const
{
  return 715;
}

float ConditionFunctions::IsUndead::Execute(
  MpActor& actor, [[maybe_unused]] uint32_t parameter1,
  [[maybe_unused]] uint32_t parameter2, const ConditionEvaluatorContext&)
{
  return HasKeyword::ActorHasKeyword(actor, kActorTypeUndead) ? 1.f : 0.f;
}
