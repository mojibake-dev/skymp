#include "IsCommandedActor.h"
#include "MpActor.h"

const char* ConditionFunctions::IsCommandedActor::GetName() const
{
  return "IsCommandedActor";
}

uint16_t ConditionFunctions::IsCommandedActor::GetFunctionIndex() const
{
  return 700;
}

float ConditionFunctions::IsCommandedActor::Execute(
  MpActor& actor, [[maybe_unused]] uint32_t parameter1,
  [[maybe_unused]] uint32_t parameter2, const ConditionEvaluatorContext&)
{
  (void)actor;
  // no summoned or reanimated actor exists on the server before M3's
  // hosts run NPCs; a player is never a commanded actor. So Azure
  // Reconstruction's "target is a summon" refuses a player (thuum
  // docs/DECISIONS.md ADR-039, playtest twenty-three)
  return 0.f;
}
