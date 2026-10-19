#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039, UESP "Skyrim Mod:Function Indices" 699:
// whether an effect running on the actor (the server's own record,
// docs/verbs/magic-effects.md) carries the keyword
class HasMagicEffectKeyword : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;
};
}
