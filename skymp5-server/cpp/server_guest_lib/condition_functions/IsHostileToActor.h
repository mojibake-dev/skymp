#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039; UESP "Skyrim Mod:Function Indices" 719.
// Between players, hostility is ADR-023's fight: the pair's record, which a
// hit between them begins and a quiet minute or walking apart ends. The
// record is symmetric, so "is the subject hostile to the other" reads as
// "are they in a fight". The vanilla cloaks' damage effects gate on it
// (FlameCloakDmg's effect 0x10CBDD), which is why a cloak does nothing to a
// peaceful neighbour in Skyrim and on the server alike (lab b-cloak-fight,
// playtest twenty-three). The other actor is the effect's other end (its
// caster for the actor it lands on, and the reverse); a condition naming a
// third actor as its parameter is not read (HYPOTHESIS: none in the masters
// does).
class IsHostileToActor : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;
};
}
