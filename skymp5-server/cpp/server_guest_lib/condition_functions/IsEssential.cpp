#include "IsEssential.h"
#include "MpActor.h"
#include "WorldState.h"
#include "libespm/espm.h"

const char* ConditionFunctions::IsEssential::GetName() const
{
  return "IsEssential";
}

uint16_t ConditionFunctions::IsEssential::GetFunctionIndex() const
{
  return 354;
}

float ConditionFunctions::IsEssential::Execute(
  MpActor& actor, [[maybe_unused]] uint32_t parameter1,
  [[maybe_unused]] uint32_t parameter2, const ConditionEvaluatorContext&)
{
  // A player is never essential (the engine's rule for the player
  // character). An NPC is essential by its base record's flag (libespm
  // NPC_.h isEssential, the ACBS flag 0x2); an actor whose base record the
  // server cannot read (a server-made base) counts as not essential.
  // HYPOTHESIS: an essential flag set by a quest alias or a template at
  // runtime is not in the record and is not seen here.
  if (actor.IsCreatedAsPlayer()) {
    return 0.f;
  }
  try {
    const auto npcData =
      espm::GetData<espm::NPC_>(actor.GetBaseId(), actor.GetParent());
    return npcData.isEssential ? 1.f : 0.f;
  } catch (const std::exception&) {
    return 0.f;
  }
}
