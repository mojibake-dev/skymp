#pragma once
#include "ConditionFunction.h"

namespace ConditionFunctions {
// thuum docs/DECISIONS.md ADR-039; UESP "Skyrim Mod:Function Indices" 448
// (CommonLibSSE-NG include/RE/T/TESCondition.h kHasPerk). The parameter is
// the perk's form id. The server holds no perks before M5 (docs/PLAN.md: the
// damage formula has no perk path), so the function answers false for every
// actor and every perk. A rider gated on HasPerk == 1 then fails its row and
// stays out, the perk-less outcome, while the spell's base effects apply:
// Reanimate Corpse's PerkDarkSoulsReanimateEffect 0xCAB63 gates on the Dark
// Souls perk 0x581DE on Target, the caster (Skyrim.esm's bytes, 2026-10-28),
// and its ReanimateFFAimed25 0x96D0B names no perk. Before this function the
// server refused such an effect as E_CONDITION_UNKNOWN (playtest 37:
// Apocalypse's 1a02e634, Reanimate Corpse 0x65BD7), the same outcome through
// a gap; a known function answering false is the rule, and M5's perks fill
// in the answer here without touching the reader or the evaluator.
class HasPerk : public ConditionFunction
{
public:
  const char* GetName() const override;

  uint16_t GetFunctionIndex() const override;

  float Execute(MpActor& actor, uint32_t parameter1, uint32_t parameter2,
                const ConditionEvaluatorContext& context) override;

  bool IsFormParameter(int index) const override;
};
}
