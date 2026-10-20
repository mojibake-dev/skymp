#pragma once
#include "Condition.h"
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class MpActor;
class ConditionFunctionMap;

enum class ConditionsEvaluatorCaller
{
  kDamageMultConditionalFormula,
  kCraft,
  // thuum docs/DECISIONS.md ADR-039: an effect's own conditions before the
  // server applies it to another player (Subject the actor it lands on,
  // Target its caster)
  kMagicEffect
};

struct ConditionsEvaluatorSettings
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("callersToLog", callersToLog);
  }

  static ConditionsEvaluatorSettings FromJson(const nlohmann::json& j);

  std::vector<std::string> callersToLog;
};

struct ConditionEvaluatorContext
{
  std::optional<uint32_t> damageSourceFormId;
  // thuum ADR-039: the pair an effect's conditions are judged for (the
  // actor it lands on and its caster), for the functions that ask about the
  // other actor (IsHostileToActor); 0 from the other callers
  uint32_t effectVictimFormId = 0;
  uint32_t effectCasterFormId = 0;
  // thuum train-29: whether the cast that landed the effect was dual cast
  // (the claimed cast's record, from the caster's game's report), for
  // EffectWasDualCast (724); false from every other caller
  bool effectWasDualCast = false;
};

class ConditionsEvaluator
{
public:
  static void EvaluateConditions(
    const ConditionFunctionMap& conditionFunctionMap,
    const ConditionsEvaluatorSettings& settings,
    ConditionsEvaluatorCaller caller, const std::vector<Condition>& conditions,
    const MpActor& aggressor, const MpActor& target,
    const std::function<void(bool, std::vector<std::string>&)>& callback,
    const ConditionEvaluatorContext& context = ConditionEvaluatorContext());

private:
  static bool EvaluateConditionsImpl(
    const ConditionFunctionMap& conditionFunctionMap,
    const std::vector<Condition>& conditions,
    std::vector<int>* outConditionResolutions,
    std::vector<float>* outConditionFunctionResults, const MpActor& aggressor,
    const MpActor& target, const ConditionEvaluatorContext& context);

  static std::vector<std::string> LogEvaluateConditionsResolution(
    const std::vector<Condition>& conditions,
    const std::vector<int>& conditionResolutions,
    const std::vector<float>& conditionFunctionResults, bool finalResult);

  static std::pair<bool, float> EvaluateCondition(
    const ConditionFunctionMap& conditionFunctionMap,
    const Condition& condition, const MpActor& aggressor,
    const MpActor& target, const ConditionEvaluatorContext& context);

  static bool CompareFloats(float a, float b, const std::string& op);

  static uint32_t ExtractParameter(const std::string& parameter1);
};
