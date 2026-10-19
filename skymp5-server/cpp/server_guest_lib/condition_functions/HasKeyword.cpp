#include "HasKeyword.h"
#include "MpActor.h"
#include "WorldState.h"
#include <algorithm>

const char* ConditionFunctions::HasKeyword::GetName() const
{
  return "HasKeyword";
}

uint16_t ConditionFunctions::HasKeyword::GetFunctionIndex() const
{
  return 560;
}

bool ConditionFunctions::HasKeyword::ActorHasKeyword(MpActor& actor,
                                                     uint32_t keywordId)
{
  auto* worldState = actor.GetParent();
  if (!worldState || keywordId == 0) {
    return false;
  }
  auto& br = worldState->GetEspm().GetBrowser();
  for (const uint32_t recordId : { actor.GetRaceId(), actor.GetBaseId() }) {
    if (recordId == 0) {
      continue;
    }
    const espm::LookupResult res = br.LookupById(recordId);
    if (!res.rec) {
      continue;
    }
    const auto keywords = res.rec->GetKeywordIds(worldState->GetEspmCache());
    if (std::any_of(keywords.begin(), keywords.end(), [&](uint32_t k) {
          return res.ToGlobalId(k) == keywordId;
        })) {
      return true;
    }
  }
  return false;
}

float ConditionFunctions::HasKeyword::Execute(
  MpActor& actor, uint32_t parameter1, [[maybe_unused]] uint32_t parameter2,
  const ConditionEvaluatorContext&)
{
  return ActorHasKeyword(actor, parameter1) ? 1.f : 0.f;
}
