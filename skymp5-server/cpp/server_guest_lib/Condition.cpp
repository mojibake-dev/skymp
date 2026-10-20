#include "Condition.h"
#include "condition_functions/ConditionFunctionFactory.h"
#include <set>
#include <spdlog/spdlog.h>

namespace {
std::string ConvertOperatorToString(espm::CTDA::Operator op)
{
  switch (op) {
    case espm::CTDA::Operator::EqualTo:
      return "==";
    case espm::CTDA::Operator::NotEqualTo:
      return "!=";
    case espm::CTDA::Operator::GreaterThen:
      return ">";
    case espm::CTDA::Operator::GreaterThenOrEqualTo:
      return ">=";
    case espm::CTDA::Operator::LessThen:
      return "<";
    case espm::CTDA::Operator::LessThenOrEqualTo:
      return "<=";
    default:
      spdlog::error("ConvertOperatorToString - Unknown operator: {}",
                    static_cast<int>(op));
      return "";
  }
}

std::string ToStringHexPrefixedUpperCase(uint32_t value)
{
  constexpr auto kValueFmt = "0x%0X";
  size_t size = std::snprintf(nullptr, 0, kValueFmt, value);

  std::vector<char> buffer;
  buffer.resize(size + 1);

  std::sprintf(buffer.data(), kValueFmt, value);

  return { buffer.begin(), buffer.begin() + size };
}

std::string ConvertLogicalOperatorToString(espm::CTDA::Flags flags)
{
  if (static_cast<uint8_t>(flags) &
      static_cast<uint8_t>(espm::CTDA::Flags::OR)) {
    return "OR";
  }
  return "AND";
}

std::string ConvertRunOnTypeToString(espm::CTDA::RunOnTypeFlags runOnType)
{
  switch (runOnType) {
    case espm::CTDA::RunOnTypeFlags::Subject:
      return "Subject";
    case espm::CTDA::RunOnTypeFlags::Target:
      return "Target";
    case espm::CTDA::RunOnTypeFlags::Reference:
      return "Reference";
    case espm::CTDA::RunOnTypeFlags::CombatTarget:
      return "CombatTarget";
    case espm::CTDA::RunOnTypeFlags::LinkedReference:
      return "LinkedReference";
    case espm::CTDA::RunOnTypeFlags::QuestAlias:
      return "QuestAlias";
    case espm::CTDA::RunOnTypeFlags::PackageData:
      return "PackageData";
    case espm::CTDA::RunOnTypeFlags::EventData:
      return "EventData";
    default:
      throw std::runtime_error("Unknown run on type");
  }
}

std::string ConvertFunctionIndexToString(uint16_t functionIndex)
{
  static auto g_conditionFunctionMap =
    ConditionFunctionFactory::CreateConditionFunctions();

  auto conditionFunction =
    g_conditionFunctionMap.GetConditionFunction(functionIndex);

  if (conditionFunction) {
    return conditionFunction->GetName();
  }
  // thuum docs/DECISIONS.md ADR-039: an index the server does not evaluate
  // is worth one line, not one per hit (playtest twenty-four: 143 of them
  // in an hour for two indices, HasPerk and EffectWasDualCast, both by
  // design: see docs/verbs/magic-effects.md)
  static std::set<uint16_t> g_unknownIndicesSeen;
  if (g_unknownIndicesSeen.insert(functionIndex).second) {
    spdlog::warn("ConvertFunctionIndexToString - Unknown function index: {} "
                 "(logged once; later ones at debug)",
                 functionIndex);
  } else {
    spdlog::debug("ConvertFunctionIndexToString - Unknown function index: {}",
                  functionIndex);
  }
  return "";
}
}

Condition Condition::FromCtda(const espm::CTDA& ctda)
{
  Condition condition;

  condition.comparison = ConvertOperatorToString(ctda.GetOperator());
  condition.value = ctda.comparisonValue;

  condition.parameter1 =
    ToStringHexPrefixedUpperCase(ctda.GetDefaultData().firstParameter);
  condition.parameter2 =
    ToStringHexPrefixedUpperCase(ctda.GetDefaultData().secondParameter);

  condition.logicalOperator = ConvertLogicalOperatorToString(ctda.GetFlags());
  condition.runsOn = ConvertRunOnTypeToString(ctda.runOnType);

  condition.function = ConvertFunctionIndexToString(ctda.functionIndex);
  condition.functionIndex = ctda.functionIndex;

  return condition;
}
