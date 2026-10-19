#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039; UESP "Skyrim Mod:Function Indices" 74:
// a global variable's value. The player's own recorded global first (thuum
// docs/verbs/player-globals.md: the values its game wrote, R2), else the
// value the records hold (libespm GLOB), else 0 with a warning. Dragonborn's
// Unrelenting Force gates its extra damage on DLC2BlackBookReward2 == 1
// (the "Dragonborn Force" reward), 0 by the records.
class GetGlobalValue : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;
};
}
