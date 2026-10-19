#include "HasMagicEffectKeyword.h"
#include "MpActor.h"
#include "WorldState.h"
#include <algorithm>

const char* ConditionFunctions::HasMagicEffectKeyword::GetName() const
{
  return "HasMagicEffectKeyword";
}

uint16_t ConditionFunctions::HasMagicEffectKeyword::GetFunctionIndex() const
{
  return 699;
}

float ConditionFunctions::HasMagicEffectKeyword::Execute(
  MpActor& actor, uint32_t parameter1, [[maybe_unused]] uint32_t parameter2,
  const ConditionEvaluatorContext&)
{
  auto* worldState = actor.GetParent();
  const auto& running = actor.GetChangeForm().runningEffects;
  if (!worldState || !running || parameter1 == 0) {
    return 0.f;
  }
  auto& br = worldState->GetEspm().GetBrowser();
  for (const auto& r : *running) {
    uint32_t effectId = 0;
    try {
      effectId = r.effect.ToFormId(worldState->espmFiles);
    } catch (std::exception&) {
      continue;
    }
    const espm::LookupResult res = br.LookupById(effectId);
    if (!res.rec) {
      continue;
    }
    const auto keywords = res.rec->GetKeywordIds(worldState->GetEspmCache());
    if (std::any_of(keywords.begin(), keywords.end(), [&](uint32_t k) {
          return res.ToGlobalId(k) == parameter1;
        })) {
      return 1.f;
    }
  }
  return 0.f;
}
