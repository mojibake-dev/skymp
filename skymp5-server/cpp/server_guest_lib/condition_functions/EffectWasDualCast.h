#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039 (the effect conditions the server
// evaluates), UESP "Skyrim Mod:Function Indices" 724 (CommonLibSSE-NG
// include/RE/T/TESCondition.h kEffectWasDualCast). Whether the cast that
// landed this effect was dual cast: the caster's game reports it
// (SpellCastMessage.isDualCasting), the cast record keeps it, and the hit's
// claim hands it to the evaluator (train-29, docs/verbs/spell-cast.md).
class EffectWasDualCast : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;
};
}
