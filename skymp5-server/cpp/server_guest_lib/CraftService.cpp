#include "CraftService.h"

#include "ConditionsEvaluator.h"
#include "CraftReportMessage.h"
#include "MpActor.h"
#include "PartOne.h"
#include "RawMessageData.h"
#include "SetInventoryMessage.h"
#include "WorldState.h"
#include "gamemode_events/CraftEvent.h"
#include <algorithm>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>
#include <vector>

CraftService::CraftService(PartOne& partOne_)
  : partOne(partOne_)
{
}

void CraftService::OnCraftItem(const RawMessageData& rawMsgData,
                               const Inventory& inputObjects,
                               uint32_t workbenchId, uint32_t resultObjectId)
{
  auto& workbench =
    partOne.worldState.GetFormAt<MpObjectReference>(workbenchId);

  auto& br = partOne.worldState.GetEspm().GetBrowser();
  auto& cache = partOne.worldState.GetEspmCache();
  auto base = br.LookupById(workbench.GetBaseId());

  spdlog::info("User {} tries to craft {:#x} on workbench {:#x}",
               rawMsgData.userId, resultObjectId, workbenchId);

  bool isFurnitureOrActivator =
    base.rec->GetType() == "FURN" || base.rec->GetType() == "ACTI";
  if (!isFurnitureOrActivator) {
    return spdlog::error("Unable to use {} as workbench",
                         base.rec->GetType().ToString());
  }

  MpActor* me = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!me) {
    return spdlog::error("Unable to craft without Actor attached");
  }

  auto workbenchBase = br.LookupById(workbench.GetBaseId());

  if (!workbenchBase.rec) {
    return spdlog::error("Workbench ref without base object {:x}",
                         workbench.GetFormId());
  }

  std::vector<uint32_t> workbenchKeywordIds =
    workbenchBase.rec->GetKeywordIds(cache);

  auto recipesList =
    FindRecipe(me, workbenchKeywordIds, br, inputObjects, resultObjectId);

  if (recipesList.empty()) {
    return spdlog::error(
      "Recipe not found: inputObjects={}, workbenchId={:#x}, "
      "resultObjectId={:#x}",
      inputObjects.ToJson().dump(), workbenchId, resultObjectId);
  }

  if (recipesList.size() > 1) {
    spdlog::warn("Found more than 1 recipe ({}), using the 1st one",
                 recipesList.size());
  }

  UseCraftRecipe(me, reinterpret_cast<const espm::COBJ*>(recipesList[0].rec),
                 cache, br, recipesList[0].fileIdx);
}

bool CraftService::RecipeItemsMatch(const espm::LookupResult& lookupRes,
                                    const Inventory& inputObjects,
                                    uint32_t resultObjectId)
{
  auto recipe = reinterpret_cast<const espm::COBJ*>(lookupRes.rec);

  espm::CompressedFieldsCache dummyCache;
  auto recipeData = recipe->GetData(dummyCache);

  enum
  {
    ArmorTable = 0xadb78,
    SharpeningWheel = 0x88108
  };
  const bool isTemper = recipeData.benchKeywordId == ArmorTable ||
    recipeData.benchKeywordId == SharpeningWheel;
  if (isTemper) {
    return false;
  }

  auto thisInputObjects = recipeData.inputObjects;
  for (auto& entry : thisInputObjects) {
    auto formId = lookupRes.ToGlobalId(entry.formId);
    if (inputObjects.GetItemCount(formId) != entry.count) {
      return false;
    }
  }
  auto formId = lookupRes.ToGlobalId(recipeData.outputObjectFormId);
  if (formId != resultObjectId) {
    return false;
  }
  return true;
}

std::vector<espm::LookupResult> CraftService::FindRecipe(
  std::optional<MpActor*> me,
  std::optional<std::vector<uint32_t>> workbenchKeywordIds,
  const espm::CombineBrowser& br, const Inventory& inputObjects,
  uint32_t resultObjectId)
{
  if (allRecipes.empty()) {
    allRecipes = br.GetDistinctRecordsByType("COBJ");
  }

  std::vector<espm::LookupResult> candidatesConsideredUsable;

  for (auto& recipe : allRecipes) {
    if (!RecipeItemsMatch(recipe, inputObjects, resultObjectId)) {
      continue;
    }

    spdlog::info("CraftService::FindRecipe - Recipe candidate found: {:x}",
                 recipe.ToGlobalId(recipe.rec->GetId()));

    const bool canBeUsed =
      ConsiderRecipeCandidate(me, workbenchKeywordIds, recipe);
    if (canBeUsed) {
      candidatesConsideredUsable.push_back(recipe);
      spdlog::info("CraftService::FindRecipe - Recipe candidate usable");
    } else {
      spdlog::info("CraftService::FindRecipe - Recipe candidate not usable");
    }
  }

  return candidatesConsideredUsable;
}

bool CraftService::ConsiderRecipeCandidate(
  std::optional<MpActor*> me,
  std::optional<std::vector<uint32_t>> workbenchKeywordIds,
  const espm::LookupResult& lookupRes)
{
  auto cobj = reinterpret_cast<const espm::COBJ*>(lookupRes.rec);
  auto cobjData = cobj->GetData(cache);

  bool finalConsiderationResult = true;

  if (me.has_value()) {
    bool evalRes = EvaluateCraftRecipeConditions(*me, cobjData);
    if (!evalRes) {
      spdlog::info("CraftService::ConsiderRecipeCandidate - Craft recipe "
                   "conditions are not met");
      finalConsiderationResult = false;
    }
  } else {
    spdlog::info("CraftService::ConsiderRecipeCandidate - Actor not "
                 "specified, skipping conditions check");
  }

  if (workbenchKeywordIds.has_value()) {
    auto recipeBenchKeywordId = lookupRes.ToGlobalId(cobjData.benchKeywordId);

    // Note: In the original game, setting the benchmark keyword to NONE
    // removes the recipe from all crafting stations.

    bool includes =
      std::any_of(workbenchKeywordIds->begin(), workbenchKeywordIds->end(),
                  [&](uint32_t id) { return id == recipeBenchKeywordId; });

    if (!includes) {
      std::vector<std::string> hexIds;
      hexIds.reserve(workbenchKeywordIds->size());
      for (auto id : *workbenchKeywordIds) {
        hexIds.push_back(fmt::format("{:x}", id));
      }

      spdlog::info("CraftService::ConsiderRecipeCandidate - Craft recipe "
                   "workbench keywords don't match: recipe one {:x} is not in "
                   "workbench ids {}",
                   recipeBenchKeywordId, fmt::join(hexIds, ", "));
      finalConsiderationResult = false;
    }

  } else {
    spdlog::info("CraftService::ConsiderRecipeCandidate - Workbench keyword "
                 "id not specified, skipping bench keyword id check");
  }

  return finalConsiderationResult;
}

void CraftService::UseCraftRecipe(MpActor* me, const espm::COBJ* recipeUsed,
                                  espm::CompressedFieldsCache& cache,
                                  const espm::CombineBrowser& br, int espmIdx)
{
  auto recipeData = recipeUsed->GetData(cache);
  auto mapping = br.GetCombMapping(espmIdx);

  spdlog::info("Using craft recipe with EDID {} from espm file with index {}",
               recipeUsed->GetEditorId(cache), espmIdx);

  std::vector<Inventory::Entry> entries;
  for (auto& entry : recipeData.inputObjects) {
    auto formId = espm::utils::GetMappedId(entry.formId, *mapping);
    entries.push_back({ formId, entry.count });
  }

  auto outputFormId =
    espm::utils::GetMappedId(recipeData.outputObjectFormId, *mapping);

  if (spdlog::should_log(spdlog::level::info)) {
    std::string s = fmt::format("User formId={:#x} crafted", me->GetFormId());
    for (const auto& entry : entries) {
      s += fmt::format(" -{:#x} x{}", entry.baseId, entry.count);
    }
    s += fmt::format(" +{:#x} x{}", outputFormId, recipeData.outputCount);
    spdlog::info("{}", s);
  }

  auto recipeId = espm::utils::GetMappedId(recipeUsed->GetId(), *mapping);

  CraftEvent craftEvent(me, outputFormId, recipeData.outputCount, recipeId,
                        entries);

  craftEvent.Fire(me->GetParent());
}

bool CraftService::EvaluateCraftRecipeConditions(
  MpActor* me, const espm::COBJ::Data& recipeData)
{
  std::vector<Condition> conditions;
  std::transform(recipeData.conditions.begin(), recipeData.conditions.end(),
                 std::back_inserter(conditions),
                 [&](const auto& ctda) { return Condition::FromCtda(ctda); });

  // TODO: aggressor and target terms are not relevant for crafting
  const MpActor& aggressor = *me;
  const MpActor& target = *me;

  bool evalRes_ = false;

  auto callback = [&](bool evalRes, std::vector<std::string>& strings) {
    evalRes_ = evalRes;

    if (!strings.empty()) {
      if (evalRes) {
        strings.insert(strings.begin(),
                       fmt::format("EvaluateConditions result is true"));
      } else {
        strings.insert(strings.begin(),
                       fmt::format("EvaluateConditions result is false"));
      }
    }
  };

  static const ConditionsEvaluatorSettings kDefaultSettings;

  static const ConditionFunctionMap kEmptyMap;

  auto worldState = me->GetParent();

  const ConditionsEvaluatorSettings& settings =
    worldState ? worldState->conditionsEvaluatorSettings : kDefaultSettings;

  const ConditionFunctionMap& conditionFunctionMap =
    worldState ? worldState->conditionFunctionMap : kEmptyMap;

  ConditionsEvaluator::EvaluateConditions(
    conditionFunctionMap, settings, ConditionsEvaluatorCaller::kCraft,
    conditions, aggressor, target, callback);

  return evalRes_;
}

namespace {
bool StandsFor(const Inventory::Entry& row, const Inventory::Entry& lost)
{
  return row.baseId == lost.baseId && row.health == lost.health &&
    row.enchantmentId == lost.enchantmentId &&
    row.maxCharge == lost.maxCharge &&
    row.removeEnchantmentOnUnequip.value_or(false) ==
    lost.removeEnchantmentOnUnequip.value_or(false) &&
    row.soul == lost.soul && row.poisonId == lost.poisonId &&
    SameCreated(row.createdBase, lost.createdBase) &&
    SameCreated(row.createdEnchantment, lost.createdEnchantment) &&
    SameCreated(row.createdPoison, lost.createdPoison);
}
}

std::optional<std::vector<Inventory::Entry>> CraftService::HeldRows(
  const Inventory& record, const std::vector<Inventory::Entry>& lost,
  size_t* firstUnheld)
{
  std::vector<uint32_t> left(record.entries.size());
  for (size_t i = 0; i < record.entries.size(); ++i) {
    left[i] = record.entries[i].count;
  }
  std::vector<Inventory::Entry> rows;
  for (size_t at = 0; at < lost.size(); ++at) {
    const auto& stack = lost[at];
    uint32_t wanted = stack.count;
    for (size_t i = 0; i < record.entries.size() && wanted > 0; ++i) {
      if (left[i] == 0 || !StandsFor(record.entries[i], stack)) {
        continue;
      }
      const uint32_t take = std::min(wanted, left[i]);
      Inventory::Entry row = record.entries[i];
      row.count = take;
      rows.push_back(row);
      left[i] -= take;
      wanted -= take;
    }
    if (wanted > 0) {
      if (firstUnheld) {
        *firstUnheld = at;
      }
      return std::nullopt;
    }
  }
  return rows;
}

namespace {
// A stack as HeldRows compares it, for a refusal's line: the base, the count
// and the lasting extras, a created form by its kind and effect count
std::string DescribeStack(const Inventory::Entry& e)
{
  std::string out = fmt::format("{:x} x{}", e.baseId, e.count);
  if (e.health) {
    out += fmt::format(" health {}", *e.health);
  }
  if (e.enchantmentId) {
    out += fmt::format(" enchantment {:x}", *e.enchantmentId);
  }
  if (e.maxCharge) {
    out += fmt::format(" maxCharge {}", *e.maxCharge);
  }
  if (e.removeEnchantmentOnUnequip.value_or(false)) {
    out += " removeEnchantmentOnUnequip";
  }
  if (e.soul) {
    out += fmt::format(" soul {}", static_cast<int>(*e.soul));
  }
  if (e.poisonId) {
    out += fmt::format(" poison {:x}", *e.poisonId);
  }
  const auto created = [&](const char* what,
                           const std::optional<CreatedForm>& form) {
    if (form) {
      out += fmt::format(" {} kind {} with {} effect(s)", what,
                         static_cast<int>(form->kind), form->effects.size());
    }
  };
  created("createdBase", e.createdBase);
  created("createdEnchantment", e.createdEnchantment);
  created("createdPoison", e.createdPoison);
  return out;
}

// The record as the answer to report `seq` (SetInventory's craftAck,
// SCHEMA_VERSION 62), sent at once: the seat ends its hold on it, never on an
// inventory sent before the report was read. The deferred inventory word the
// record's edits queued follows with the same rows
void SendCraftAnswer(MpActor& me, uint32_t seq)
{
  SetInventoryMessage answer;
  answer.inventory = me.GetInventory();
  answer.craftAck = seq;
  me.SendToUser(answer, true);
}
}

void CraftService::OnCraftReport(const RawMessageData& rawMsgData,
                                 const CraftReportMessage& msg)
{
  MpActor* me = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!me) {
    return spdlog::warn("OnCraftReport - no actor for user {}",
                        rawMsgData.userId);
  }

  // the worn flag is the seat's own, never the record's (OnPutItem's rule)
  std::vector<Inventory::Entry> lost = msg.lost;
  std::vector<Inventory::Entry> gained = msg.gained;
  for (auto& stack : lost) {
    stack.SetWorn(Inventory::Worn::None);
  }
  for (auto& stack : gained) {
    stack.SetWorn(Inventory::Worn::None);
  }

  size_t unheld = 0;
  const auto rows = HeldRows(me->GetInventory(), lost, &unheld);
  if (!rows) {
    spdlog::info(
      "[verb:craft] E_CRAFT_NOT_HELD: user {} actor {:x} at "
      "{:x}, report {}: the record does not hold lost stack {} "
      "({}), the sitting refused ({} stack(s) lost, {} gained); "
      "the record goes back",
      rawMsgData.userId, me->GetFormId(), msg.workbench, msg.seq, unheld,
      unheld < lost.size() ? DescribeStack(lost[unheld]) : std::string("?"),
      msg.lost.size(), msg.gained.size());
    SendCraftAnswer(*me, msg.seq);
    return;
  }

  try {
    if (!rows->empty()) {
      me->RemoveItems(*rows);
    }
    // R2: what the crafter's engine made, as it reported it (recorded
    // unvalidated; a created form's effects are its menu's numbers)
    if (!gained.empty()) {
      me->AddItems(gained);
    }
  } catch (std::exception& e) {
    spdlog::error("[verb:craft] E_CRAFT_APPLY: user {} actor {:x} at {:x}, "
                  "report {}: {}",
                  rawMsgData.userId, me->GetFormId(), msg.workbench, msg.seq,
                  e.what());
    SendCraftAnswer(*me, msg.seq);
    return;
  }

  // a disenchant destroys a stack's item and teaches its game-file
  // enchantment or that enchantment's base (HYPOTHESIS, the static read of
  // tracker #98 note 3924). Which one the stack carried is the engine's: a
  // weapon's or an armor's own record names it (libespm's WEAP reads no
  // EITM), so the server holds each to an ENCH of the files and the sitting
  // to a lost stack that can carry one, and records the rest unvalidated
  size_t learned = 0;
  if (!msg.learnedEnchantments.empty() && partOne.worldState.HasEspm()) {
    auto& br = partOne.worldState.GetEspm().GetBrowser();
    const auto typeOf = [&](uint32_t formId) {
      const auto lookup = br.LookupById(formId);
      return lookup.rec ? lookup.rec->GetType().ToString() : std::string();
    };
    const bool lostEnchantable =
      std::any_of(lost.begin(), lost.end(), [&](const Inventory::Entry& e) {
        if (e.enchantmentId) {
          return true;
        }
        const auto type = typeOf(e.baseId);
        return type == "WEAP" || type == "ARMO";
      });
    for (const uint32_t enchantment : msg.learnedEnchantments) {
      if (typeOf(enchantment) != "ENCH") {
        spdlog::info("[verb:craft] E_CRAFT_LEARNED_FORM: user {} actor {:x} "
                     "learned {:x}, which is no enchantment of the files",
                     rawMsgData.userId, me->GetFormId(), enchantment);
        continue;
      }
      if (!lostEnchantable) {
        spdlog::info("[verb:craft] E_CRAFT_LEARNED_NO_ITEM: user {} actor "
                     "{:x} learned {:x} in a sitting that lost no "
                     "enchantable stack",
                     rawMsgData.userId, me->GetFormId(), enchantment);
        continue;
      }
      if (me->RecordKnownEnchantment(
            FormDesc::FromFormId(enchantment, partOne.worldState.espmFiles))) {
        ++learned;
      }
    }
  }

  SendCraftAnswer(*me, msg.seq);
  spdlog::info("[verb:craft] user {} actor {:x} at {:x}, report {}: {} "
               "stack(s) lost, {} gained, {} enchantment(s) newly known",
               rawMsgData.userId, me->GetFormId(), msg.workbench, msg.seq,
               lost.size(), gained.size(), learned);
}
