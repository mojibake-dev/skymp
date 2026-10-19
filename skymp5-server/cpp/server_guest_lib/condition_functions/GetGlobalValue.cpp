#include "GetGlobalValue.h"
#include "MpActor.h"
#include "WorldState.h"
#include "libespm/GLOB.h"
#include "libespm/Loader.h"
#include <spdlog/spdlog.h>

const char* ConditionFunctions::GetGlobalValue::GetName() const
{
  return "GetGlobalValue";
}

uint16_t ConditionFunctions::GetGlobalValue::GetFunctionIndex() const
{
  return 74;
}

float ConditionFunctions::GetGlobalValue::Execute(
  MpActor& actor, uint32_t parameter1, [[maybe_unused]] uint32_t parameter2,
  const ConditionEvaluatorContext&)
{
  WorldState* worldState = actor.GetParent();
  if (!worldState) {
    return 0.f;
  }
  for (const auto& global : actor.GetPlayerGlobals()) {
    if (global.global.ToFormId(worldState->espmFiles) == parameter1) {
      return global.value;
    }
  }
  try {
    return espm::GetData<espm::GLOB>(parameter1, worldState).value;
  } catch (std::exception& e) {
    spdlog::warn("GetGlobalValue - global {:x} is in no record and no "
                 "player's globals: {}; 0",
                 parameter1, e.what());
    return 0.f;
  }
}
