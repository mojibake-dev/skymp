#pragma once
#include "Inventory.h"
#include "libespm/Loader.h"
#include <cstdint>
#include <optional>
#include <vector>

class PartOne;
class WorldState;
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

  // One sitting at a crafting bench, as the crafter's engine computed it;
  // the server checks consistency only. The lost stacks are taken from the
  // record when it holds every one (HeldRows), else nothing changes, the
  // refusal is logged with its reason code and the record goes back to the
  // client; the gained stacks are added as reported, recorded unvalidated
  // (no recipe is looked up: brewing, tempering, enchanting and a forge's
  // recipes alike); the learned enchantments are kept on the player's record
  // when each is an ENCH of the files and the sitting lost a stack that can
  // carry one. The legacy CraftItem path above is kept; the client does not
  // send it
  void OnCraftReport(const RawMessageData& rawMsgData,
                     const CraftReportMessage& msg);

  // public for CraftReportTest.cpp: the stack's created forms as the engine
  // compares them, an effect's magnitude, area or duration zeroed where its
  // MGEF's record flags NoMagnitude, NoArea or NoDuration (libespm MGEF.h;
  // assumes the engine's compare at 0x1401430f0 skips them). The platform's
  // natives zero them at the source; this keeps a report from an older
  // client from splitting one form into two rows
  static void NormalizeCreated(WorldState& worldState, Inventory::Entry& e);

  // public for CraftReportTest.cpp: the record's own rows the reported lost
  // stacks stand for, each with the count taken from it, or nullopt when
  // the record does not hold every one. A row stands for a lost stack with
  // the same base, created definitions and lasting extras (health, the
  // enchantment and its maximum charge, the soul, the poison); the charge
  // left, a poison's doses left and the name are the engine's of the moment
  // and the worn flag is the client's, so none of them is compared. With
  // `firstUnheld`, the index in `lost` of the first stack the record does
  // not hold is written there on a refusal
  static std::optional<std::vector<Inventory::Entry>> HeldRows(
    const Inventory& record, const std::vector<Inventory::Entry>& lost,
    size_t* firstUnheld = nullptr);

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
