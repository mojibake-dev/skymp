#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039 (the effect conditions the server
// evaluates), UESP "Skyrim Mod:Function Indices" 14 (CommonLibSSE-NG
// include/RE/T/TESCondition.h kGetActorValue). Hailstone's base effect
// gates on Subject.GetActorValue(ResistFrost) > 0 (train-30,
// docs/verbs/spell-cast.md). The parameter is the actor value's index
// (CommonLibSSE-NG include/RE/A/ActorValues.h: Health 24, ResistFrost 43).
class GetActorValue : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;
};
}
