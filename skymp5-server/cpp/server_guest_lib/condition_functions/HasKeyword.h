#pragma once
#include "ConditionFunction.h"
#include <cstdint>

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039, UESP "Skyrim Mod:Function Indices" 560:
// whether the actor carries the keyword, by its race record's and its base
// record's KWDA (a player's race keywords: ActorTypeNPC, a vampire race's
// ActorTypeUndead); a hosted NPC's base carries its own
class HasKeyword : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;

  // the shared read: whether `actor`'s race or base names `keywordId`
  static bool ActorHasKeyword(MpActor& actor, uint32_t keywordId);
};
}
