#include "HasPerk.h"

const char* ConditionFunctions::HasPerk::GetName() const
{
  return "HasPerk";
}

uint16_t ConditionFunctions::HasPerk::GetFunctionIndex() const
{
  return 448;
}

bool ConditionFunctions::HasPerk::IsFormParameter(int index) const
{
  // the perk is a form, so the reader maps a mod's own index space to the
  // global id as for every form parameter; the answer ignores it until M5
  return index == 1;
}

float ConditionFunctions::HasPerk::Execute(
  [[maybe_unused]] MpActor& actor, [[maybe_unused]] uint32_t parameter1,
  [[maybe_unused]] uint32_t parameter2, const ConditionEvaluatorContext&)
{
  // The server holds no perks yet (M5), so no actor has any perk here and a
  // perk-gated effect never applies server-side. Nothing is logged per call:
  // every hit of a perk-rider spell evaluates this row.
  return 0.f;
}
