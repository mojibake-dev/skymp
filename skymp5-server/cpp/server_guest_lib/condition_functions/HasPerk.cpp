#include "HasPerk.h"
#include "ConditionsEvaluator.h"

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
  [[maybe_unused]] uint32_t parameter2,
  const ConditionEvaluatorContext& context)
{
  // The server holds no perks yet (M5). For a recipe (the crafting caller)
  // the perk gate is the player's own game's: its menu offered only what its
  // perks allow, so the gate is taken as met, which is what crafting did
  // before this function existed (an unknown function read true there; the
  // overnight train's T0 of 2026-10-29 lost CraftTest's steel recipe to a
  // false answer). For an effect landing on another actor (the magic caller)
  // the server's own answer is none: a perk rider never applies server-side,
  // as it was refused before. Nothing is logged per call: every hit of a
  // perk-rider spell evaluates this row.
  return context.caller == ConditionsEvaluatorCaller::kCraft ? 1.f : 0.f;
}
