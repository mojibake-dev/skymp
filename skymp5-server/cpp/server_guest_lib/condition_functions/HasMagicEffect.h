#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039, UESP "Skyrim Mod:Function Indices" 214
// (CommonLibSSE-NG include/RE/T/TESCondition.h kHasMagicEffect).
// Hailstone's Shatter effect gates on Subject.HasMagicEffect(Hailstone's
// base effect) == 0 (train-30, docs/verbs/spell-cast.md). The parameter is
// the magic effect's form id.
class HasMagicEffect : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;

  bool IsFormParameter(int index) const override;
};
}
