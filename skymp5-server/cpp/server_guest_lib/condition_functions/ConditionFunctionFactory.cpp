#include "ConditionFunctionFactory.h"

#include "GetActorValuePercent.h"
#include "GetEquipped.h"
#include "GetEquippedItemType.h"
#include "GetIsPlayableRace.h"
#include "GetIsRace.h"
#include "GetItemCount.h"
#include "HasSpell.h"
#include "IsBlocking.h"
#include "IsInInterior.h"
#include "IsWeaponMagicOut.h"
#include "IsWeaponOut.h"
#include "SpellHasKeyword.h"
#include "WornApparelHasKeywordCount.h"
#include "WornHasKeyword.h"

// thuum docs/DECISIONS.md ADR-039: the effect conditions the server evaluates
#include "GetDead.h"
#include "GetGlobalValue.h"
#include "GetIsID.h"
#include "HasKeyword.h"
#include "HasMagicEffectKeyword.h"
#include "IsActor.h"
#include "IsCommandedActor.h"
#include "IsHostileToActor.h"
#include "IsUndead.h"

#include "EffectWasDualCast.h"
#include "IsEssential.h"
#include "SkympGetDamageSourceHasKeyword.h"
#include "SkympGetIsDamageSource.h"
#include "SkympWornHasKeywordCount.h"

ConditionFunctionMap ConditionFunctionFactory::CreateConditionFunctions()
{
  ConditionFunctionMap res;

  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::GetEquipped>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::GetIsRace>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::GetIsPlayableRace>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::GetItemCount>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::HasSpell>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::WornHasKeyword>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::GetActorValuePercent>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::IsWeaponMagicOut>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::IsWeaponOut>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::SpellHasKeyword>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::WornApparelHasKeywordCount>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::IsInInterior>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::GetEquippedItemType>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::IsBlocking>());

  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::GetDead>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::HasKeyword>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::HasMagicEffectKeyword>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::IsActor>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::IsCommandedActor>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::IsHostileToActor>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::GetIsID>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::GetGlobalValue>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::IsUndead>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::IsEssential>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::EffectWasDualCast>());

  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::SkympGetDamageSourceHasKeyword>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::SkympGetIsDamageSource>());
  res.RegisterConditionFunction(
    std::make_shared<ConditionFunctions::SkympWornHasKeywordCount>());

  return res;
}
