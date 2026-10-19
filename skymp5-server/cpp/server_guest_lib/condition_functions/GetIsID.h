#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039; UESP "Skyrim Mod:Function Indices" 72:
// whether the actor's base record is the one named. Dragonborn.esm gates
// Unrelenting Force's extra damage on GetIsID(Player 0x7) == 1 on Target,
// the caster being a player's actor (every player's actor here has the
// Player base, MpActor's default); a summon or an NPC will not.
class GetIsID : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;
};
}
