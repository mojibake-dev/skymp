#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039, UESP "Skyrim Mod:Function Indices" 715:
// the engine's IsUndead is the ActorTypeUndead keyword on the actor
// (Skyrim.esm 0x13796, thuum lab/esm.py: the vampire races carry it,
// docs/private/decisions-m2-apocalypse.md section 4); a player's
// vampirism is undeclared state until its M5 verb, so a vampire player
// reads undead only through its race
class IsUndead : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;
};
}
