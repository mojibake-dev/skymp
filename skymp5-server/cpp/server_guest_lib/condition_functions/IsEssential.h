#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039 (the effect conditions the server
// evaluates), UESP "Skyrim Mod:Function Indices" 354 (CommonLibSSE-NG
// include/RE/T/TESCondition.h kIsEssential). Finger of Death's one effect
// gates on IsEssential == 0 (train-29, docs/verbs/spell-cast.md).
class IsEssential : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;
};
}
