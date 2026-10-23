#include "Fates.h"
#include "FridaHookHandler.h"
#include "game/Offsets.h"
#include <cstdint>
#include <cstring>

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the two detours
// that make the engine roll what the server rolled, docs/research/m3/
// re-npc-roll.md section 3 (a) and (c) and section 7's recommendation. Every
// engine prototype below is the re-analyst's REA read of 1.6.1170 on sky-re
// (2026-10-23, section 6's table; the ids resolved by `just addr`) or
// CommonLibSSE-NG's own declaration, cited where it is used: none is written
// from memory.
//
// HYPOTHESIS, the whole file, until the lab read x-npc-fate-roll (section 5's
// Frida plan, lab/frida/npc-roll.js) shows these five on a seat; until then
// the TS side leaves setActorFateEnabled off and both detours pass every call
// through (section 7):
//   1. 14374 runs once per placement inside InitItemImpl at the cell's load
//      and never at 3D load (else the pin moves off 14752);
//   2. 14752's out array holds exactly one NPC_ after the engine's own roll
//      (the detour's out shape: one CALCED_OBJECT, count 1);
//   3. 37323 on a live, loaded actor re-rolls without a crash or a missing
//      body (else shape B falls back to disable, native, enable);
//   4. tagged entries suppress InitOutfitItems across process transitions
//      and a ResetInventory (else the outfit detour must also guard 418622);
//   5. a reloaded seat keeps the imposed base without a new 14374
//      (persistence through the reference's change form).
namespace {
// (a) 14752 (HYPOTHESIS name: the per-reference roll wrapper), `(RE::
// TESLeveledList* a_list, RE::TESObjectREFR* a_ref, RE::BSScrapArray<RE::
// CALCED_OBJECT>* a_out)`, 14374's one call for the LVLN pick at InitItemImpl
// (re-npc-roll.md section 1, step 1): it reads the reference's
// LevCreaModifier, floors the level and calls CalculateCurrentFormList(a_list,
// level, 1, *a_out, arg5, false) (CommonLibSSE-NG include/RE/T/
// TESLeveledList.h:65). Its caller reads a_out, not a return value: no pick,
// or a pick that is not an NPC_, deletes the placement; a pick is cloned,
// tagged ExtraLeveledCreature and swapped in as the base (steps 2 to 6). The
// detour is typed with a word-sized return so a pass-through hands the caller
// the original's rax whatever the true return type is; the fated path returns
// 0. RE this: the pseudocode's return type (section 1 names none).
using LeveledRollFn =
  std::uintptr_t (*)(RE::TESLeveledList* list, RE::TESObjectREFR* ref,
                     RE::BSScrapArray<RE::CALCED_OBJECT>* out);
LeveledRollFn g_leveledRollOriginal = nullptr;

// For a fated reference the engine's roll is replaced by the server's: a_out
// is emptied and the server's NPC_ written as its one CALCED_OBJECT (include/
// RE/T/TESLeveledList.h:27-37: form, count, two pads, a ContainerItemExtra
// whose default is owner null and healthMult 100, src/RE/C/
// ContainerItemExtra.cpp:10-19), through CommonLib's own BSScrapArray API:
// BSTArray::clear (include/RE/B/BSTArray.h:518), emplace_back (:544-553,
// value-initializing the element), the alias BSScrapArray (:652), the scrap
// allocator (:285-371, whose allocate takes the calling thread's scrap heap
// when the array has none yet, src/RE/B/BSTArray.cpp:14-26) so the caller's
// own destructor frees what the detour grew (HYPOTHESIS: the caller's array
// is fresh and the engine's BSScrapArray matches the header's 0x18 allocator
// plus the size word). A fate of none leaves a_out empty: the engine deletes
// the placement (step 1), consistent with the server holding no actor there.
// A base this game cannot find is a load-order mismatch, not a roll: the
// warn names it and the engine's own roll stands.
std::uintptr_t LeveledRollReplacement(RE::TESLeveledList* list,
                                      RE::TESObjectREFR* ref,
                                      RE::BSScrapArray<RE::CALCED_OBJECT>* out)
{
  Fates::Fate fate;
  // TESForm::formID (include/RE/T/TESForm.h:354)
  if (!Fates::Enabled() || !ref || !out || !Fates::Get(ref->formID, fate)) {
    return g_leveledRollOriginal(list, ref, out);
  }
  RE::TESNPC* npc = nullptr;
  if (fate.baseId != 0) {
    // LookupByID<T> is As<T> on the form (include/RE/T/TESForm.h:223-226),
    // TESNPC::FORMTYPE = FormType::NPC (include/RE/T/TESNPC.h:42)
    npc = RE::TESForm::LookupByID<RE::TESNPC>(fate.baseId);
    if (!npc) {
      logger::warn("[verb:npc-fate] fate for {:x} names {:x}, not an NPC_ in "
                   "this game: the engine's roll stands",
                   ref->formID, fate.baseId);
      return g_leveledRollOriginal(list, ref, out);
    }
  }
  out->clear();
  if (npc) {
    auto& pick = out->emplace_back();
    pick.form = npc;
    pick.count = 1;
    pick.pad0A = 0;
    pick.pad0C = 0;
    logger::info("[verb:npc-fate] pinned {:x} to {:x}", ref->formID,
                 npc->formID);
  } else {
    logger::info("[verb:npc-fate] pinned {:x} to none: the placement is the "
                 "engine's to delete",
                 ref->formID);
  }
  return 0;
}

// (c) 16072 InventoryChanges::InitOutfitItems, CommonLibSSE-NG's own name and
// binding: `void (RE::InventoryChanges* a_this, RE::BGSOutfit* a_outfit,
// std::uint16_t a_npcLevel)` (include/RE/I/InventoryChanges.h:48; src/RE/I/
// InventoryChanges.cpp:147-152 binds the AE id 16072). The outfit equipper
// 418622 calls it while HasOutfitItems is false (re-npc-roll.md section 2):
// it rolls every LVLI of the outfit at the NPC's level and adds the picks
// tagged ExtraOutfitItem{outfit} (the engine's tag setter 11647), the tag
// AddWornOutfit (19692) equips by and RemoveOutfitItems (19690) removes by.
// For a fated owner the detour adds the server's worn list under the same tag
// and skips the roll, so HasOutfitItems turns true and the engine's next
// equipper pass equips the server's items and rolls nothing (section 3 (c);
// observation 4 above).
using InitOutfitItemsFn = void (*)(RE::InventoryChanges* changes,
                                   RE::BGSOutfit* outfit,
                                   std::uint16_t npcLevel);
InitOutfitItemsFn g_initOutfitItemsOriginal = nullptr;

// The tag's route. Two were open (section 3 (c)): the engine's tag setter
// 11647 `(ExtraDataList*, BGSOutfit*)`, a HYPOTHESIS signature, or an
// ExtraOutfitItem built on CommonLibSSE-NG's type. The second is taken: every
// fact in it is a header's (BSExtraData::Create, include/RE/B/BSExtraData.h:
// 22, zeroed memory with the vtable written, src/RE/B/BSExtraData.cpp:31-37;
// the vtable symbol VTABLE_ExtraOutfitItem, include/RE/Offsets_VTABLE.h:95;
// the outfit id at 0x10 and sizeof 0x18, include/RE/E/ExtraOutfitItem.h:
// 20-23; the type kOutfitItem, include/RE/E/ExtraDataTypes.h:316), so no
// hypothesized prototype is called, and the list it goes into is made and
// filled the way this plugin's AddItemEx already makes one for
// AddObjectToContainer (PapyrusTESModPlatform.cpp:597-610 the list, :630-657
// the insert). The engine takes the list with the entry.
RE::ExtraDataList* CreateTaggedList(RE::FormID outfitId)
{
  // The list: CommonLibSSE-NG declares ExtraDataList() (include/RE/E/
  // ExtraDataList.h:136) and the pin's sources do not define it; under
  // ENABLE_SKYRIM_AE the type carries no lock member (:207-209: the lock
  // sits at 0x18 on AE .629 and later, reached through GetLock(), :202), so
  // the engine's size is the fork's constant: BaseExtraList's data and
  // presence pointers (:18-38) and the lock, all zeroed (a zero
  // BSReadWriteLock is unlocked; include/RE/B/BSAtomic.h:70).
  constexpr size_t kBaseExtraListSizeMax = 24;
  constexpr size_t kSpinLockSizeMax = 8;
  constexpr size_t kExtraListSize = kBaseExtraListSizeMax + kSpinLockSizeMax;
  auto* memory = static_cast<uint8_t*>(RE::malloc(kExtraListSize));
  // the presence bitfield, 0x18 bytes (include/RE/E/ExtraDataList.h:21-28)
  auto* presence = RE::malloc<RE::BaseExtraList::PresenceBitfield>();
  // the extra: Create zeroes it and writes the vtable (BSExtraData.cpp:31-37)
  auto* tag = static_cast<RE::ExtraOutfitItem*>(RE::BSExtraData::Create(
    sizeof(RE::ExtraOutfitItem), RE::VTABLE_ExtraOutfitItem[0].address()));
  if (!memory || !presence || !tag) {
    return nullptr;
  }
  std::memset(memory, 0, kExtraListSize);
  std::memset(presence, 0, sizeof(*presence));
  auto* list = reinterpret_cast<RE::ExtraDataList*>(memory);
  list->_extraData.GetPresence() = presence;
  tag->id = outfitId;
  {
    // the insert (PapyrusTESModPlatform.cpp:643-657): the extra at the head
    // under the list's write lock (include/RE/B/BSAtomic.h:127), its
    // presence bit set (bits[type >> 3], 1 << (type % 8))
    RE::BSWriteLockGuard locker(list->GetLock());
    tag->next = list->_extraData.GetData();
    list->_extraData.GetData() = tag;
    const auto type = static_cast<uint32_t>(RE::ExtraDataType::kOutfitItem);
    presence->bits[type >> 3] |= static_cast<uint8_t>(1u << (type % 8));
  }
  return list;
}

void InitOutfitItemsReplacement(RE::InventoryChanges* changes,
                                RE::BGSOutfit* outfit, std::uint16_t npcLevel)
{
  Fates::Fate fate;
  // InventoryChanges::owner (include/RE/I/InventoryChanges.h:64)
  RE::TESObjectREFR* owner = changes ? changes->owner : nullptr;
  if (!Fates::Enabled() || !owner || !outfit ||
      !Fates::Get(owner->formID, fate)) {
    g_initOutfitItemsOriginal(changes, outfit, npcLevel);
    return;
  }
  size_t added = 0;
  for (const auto wornId : fate.worn) {
    // an inventory entry needs a bound object (TESObjectREFR::
    // AddObjectToContainer's first argument, include/RE/T/TESObjectREFR.h:
    // 286); LookupByID<TESBoundObject> is As<TESBoundObject> on the form
    // (include/RE/T/TESForm.h:223-226)
    auto* bound = RE::TESForm::LookupByID<RE::TESBoundObject>(wornId);
    if (!bound) {
      logger::warn("[verb:npc-fate] worn item {:x} for {:x} is no bound "
                   "object in this game, skipped",
                   wornId, owner->formID);
      continue;
    }
    auto* tagged = CreateTaggedList(outfit->formID);
    if (!tagged) {
      logger::warn("[verb:npc-fate] no memory for {:x}'s outfit tag, the "
                   "rest of the list skipped",
                   owner->formID);
      break;
    }
    // TESObjectREFR::AddObjectToContainer, vfunc 0x5A (include/RE/T/
    // TESObjectREFR.h:286; Actor's override include/RE/A/Actor.h:304): one
    // entry, count 1, no source container; the list is the entry's from here
    // (the AddItemEx idiom, PapyrusTESModPlatform.cpp:758-760)
    owner->AddObjectToContainer(bound, tagged, 1, nullptr);
    ++added;
  }
  logger::info("[verb:npc-fate] dressed {:x} from the server's roll: {} of {} "
               "worn items tagged for outfit {:x} (npc level {}), the "
               "engine's outfit roll skipped",
               owner->formID, added, fate.worn.size(), outfit->formID,
               npcLevel);
}
}

void Fates::InstallHooks()
{
  if (!REL::Module::IsAE()) {
    return;
  }
  auto* handler = Frida::HookHandler::GetSingleton();
  const bool roll =
    handler->Replace(Offsets::Hooks::LeveledRoll.address(),
                     reinterpret_cast<void*>(&LeveledRollReplacement),
                     reinterpret_cast<void**>(&g_leveledRollOriginal));
  const bool outfit =
    handler->Replace(Offsets::Hooks::InitOutfitItems.address(),
                     reinterpret_cast<void*>(&InitOutfitItemsReplacement),
                     reinterpret_cast<void**>(&g_initOutfitItemsOriginal));
  logger::info("[verb:npc-fate] detours installed, inert until "
               "setActorFateEnabled(true): leveled roll {}, outfit init {}",
               roll, outfit);
}
