#pragma once
#include "Inventory.h"
#include "libespm/Loader.h"
#include <cstdint>
#include <optional>
#include <vector>

class PartOne;
struct RawMessageData;
class MpActor;
struct CraftReportMessage;

class CraftService
{
public:
  explicit CraftService(PartOne& partOne_);

  void OnCraftItem(const RawMessageData& rawMsgData,
                   const Inventory& inputObjects, uint32_t workbenchId,
                   uint32_t resultObjectId);

  // thuum tracker #98 (crafting; Eli's decision of 2026-11-16: relay the
  // crafter's record): one sitting at a crafting bench, R2 (ADR-063: the
  // crafter's engine computed it; the server checks consistency only). The
  // lost stacks are taken from the record when it holds every one
  // (HeldRows), else nothing changes, the refusal is logged with its reason
  // code and the record goes back to the seat; the gained stacks are added
  // as reported, recorded unvalidated (no recipe is looked up: brewing,
  // tempering, enchanting and a forge's recipes alike); the learned
  // enchantments are kept on the player's record when each is an ENCH of
  // the files and the sitting lost a stack that can carry one. The legacy
  // CraftItem path above is left as it was; the client no longer sends it
  void OnCraftReport(const RawMessageData& rawMsgData,
                     const CraftReportMessage& msg);

  // public for CraftReportTest.cpp: the record's own rows the reported lost
  // stacks stand for, each with the count taken from it, or nullopt when
  // the record does not hold every one. A row stands for a lost stack with
  // the same base, created definitions and lasting extras (health, the
  // enchantment and its maximum charge, the soul, the poison); the charge
  // left, a poison's doses left and the name are the engine's of the moment
  // and the worn flag is the seat's, so none of them is compared
  static std::optional<std::vector<Inventory::Entry>> HeldRows(
    const Inventory& record, const std::vector<Inventory::Entry>& lost);

  // public for CraftTest.cpp
  bool RecipeItemsMatch(const espm::LookupResult& lookupRes,
                        const Inventory& inputObjects,
                        uint32_t resultObjectId);

  // public for CraftTest.cpp
  std::vector<espm::LookupResult> FindRecipe(
    std::optional<MpActor*> me,
    std::optional<std::vector<uint32_t>> workbenchKeywordIds,
    const espm::CombineBrowser& br, const Inventory& inputObjects,
    uint32_t resultObjectId);

private:
  bool ConsiderRecipeCandidate(
    std::optional<MpActor*> me,
    std::optional<std::vector<uint32_t>> workbenchKeywordIds,
    const espm::LookupResult& lookupRes);

  void UseCraftRecipe(MpActor* me, const espm::COBJ* recipeUsed,
                      espm::CompressedFieldsCache& cache,
                      const espm::CombineBrowser& br, int espmIdx);

  bool EvaluateCraftRecipeConditions(MpActor* me,
                                     const espm::COBJ::Data& recipeData);

  PartOne& partOne;
  std::vector<espm::LookupResult> allRecipes;
  espm::CompressedFieldsCache cache;
};
