#include "Fates.h"
#include "FridaHookHandler.h"
#include "game/Offsets.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the two detours
// that make the engine roll what the server rolled, docs/research/m3/
// re-npc-roll.md section 3 (a) for the species and section 8.4 for the
// outfit. Section 8 replaces section 3 (c) and the second detour of section
// 7: on 1.6.1170 the load-time outfit roll never passes InitOutfitItems
// (16072); InitLeveledItems rolls both outfits through the outfit-array
// resolver 16155 at the inventory's init, and 16072 is the late path from
// the outfit equipper. Every engine prototype below is the re-analyst's REA
// read of 1.6.1170 on sky-re (2026-10-23, sections 6 and 8.5; the ids
// resolved by `just addr`) or CommonLibSSE-NG's own declaration, cited where
// it is used: none is written from memory.
//
// The leveled detour's observations, HELD three times on 2026-10-23
// (x-npc-fate-roll 232048, v2 234133, v3 235610; docs/LOG.md):
//   1. 14374 runs once per placement inside InitItemImpl at the cell's load
//      and never at 3D load (the pin stays on 14752);
//   2. 14752's out array holds exactly one NPC_ after the engine's own roll
//      (the detour's out shape: one CALCED_OBJECT, count 1).
// MEASURED there as well: every load is a fresh engine roll (section 7's
// persistence observation NOT HELD), so the pin applies at every load and a
// fate that arrives late goes through rerollActor or redressActor.
//
// The outfit detour's expected chain per fresh actor (section 8.4),
// HYPOTHESIS until the 8.4 trace (lab/frida/npc-roll.js with 16129, 16155,
// 16038 and 418622's outfit id) shows it on a seat: InitInventoryIfRequired
// (16038) from InitItemImpl (19507); RemoveOutfitItems(nullptr) (19690)
// returning to 16038+0xb4, which strips every outfit-tagged entry before the
// roll (8.3); InitLeveledItems (16129); 16155 once per non-null outfit with
// its CalculateCurrentFormList (14751) calls returning to 16155+0xca; then
// the outfit equipper (418622) at the biped init with HasOutfitItems true
// and no InitOutfitItems (16072). Falsifiers: a 16155 return outside 16129
// and 16072; a 14751 return into 16155 with no 16155 entry on the thread;
// tagged entries surviving a 19690 from 16038. Until the switch is on
// (setActorFateEnabled, off by default) both detours pass every call
// through.
//
// Declared gap: the sleep outfit. 16129's second 16155 call, for TESNPC::
// sleepOutfit, falls through to the engine's roll, so a fated actor sleeps
// in the engine's picks (the server's record carries no sleep list). Open
// question: AddWornOutfit (19692) counted zero across 65 418622 calls in the
// lab read while the actors came out dressed; what wears the outfit when it
// stays zero is the trace's EquipObject (38894) question, and the redress
// native's AddWornOutfit call stands on CommonLib's SetDefaultOutfit
// sequence (src/RE/A/Actor.cpp:1016-1029) until it is answered.
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
    // LookupByID<T> is As<T> on the form (include/RE/T/TESForm.h:223-227),
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

// (c) 16155, the outfit-array resolver (re-npc-roll.md 8.2; its callers and
// registers CONFIRMED-STATIC, unnamed in CommonLibSSE-NG): `bool (RE::
// BSTArray<RE::TESForm*>* a_items, OutfitInitCtx* a_ctx)`, rcx the outfit's
// item array (BGSOutfit::outfitItems, include/RE/B/BGSOutfit.h:43, a
// BSTArray<TESForm*>, include/RE/B/BSTArray.h:373-377), rdx the context its
// two callers build on their stack; it returns MOV EAX,1 (8.2), hence the
// bool. Called by InventoryChanges::InitLeveledItems (16129, CommonLib's
// binding src/RE/I/InventoryChanges.cpp:140-145) once for the base's
// defaultOutfit and once for its sleepOutfit at the inventory's init, and by
// InventoryChanges::InitOutfitItems (16072, :147-152), the late path from
// the outfit equipper 418622 (8.1). Per item it rolls an LVLI through
// CalculateCurrentFormList and tags the picks, or adds a plain form tagged,
// a form whose entry already carries this outfit's tag skipped (8.2). For a
// fated owner and its default outfit the detour answers the server's worn
// list through the engine's own tag setter and entry adder and returns true
// without touching a_items; the one site covers the load (16129), every
// later InitInventoryIfRequired (which strips the tags first, 8.3) and the
// late re-dress (16072 from 418622): idempotent by construction (8.4).
struct OutfitInitCtx
{
  RE::BGSOutfit* outfit;         // 00, [RBP-0x31] in 16129 (8.1)
  RE::InventoryChanges* changes; // 08, [RBP-0x29]: this, the InventoryChanges
  std::uint16_t level;           // 10, [RBP-0x21]: the NPC's level, u16
};
// the layout {BGSOutfit* outfit; InventoryChanges* changes; uint16 level},
// CONFIRMED-STATIC by both callers' stores and 16155's reads (re-npc-roll.md
// 8.2, the struct named in 8.4)
static_assert(offsetof(OutfitInitCtx, outfit) == 0x0);
static_assert(offsetof(OutfitInitCtx, changes) == 0x8);
static_assert(offsetof(OutfitInitCtx, level) == 0x10);

using OutfitResolveFn = bool (*)(RE::BSTArray<RE::TESForm*>* items,
                                 OutfitInitCtx* ctx);
OutfitResolveFn g_outfitResolveOriginal = nullptr;

// 11647, the engine's ExtraOutfitItem setter (re-npc-roll.md 8.2,
// CONFIRMED-STATIC): `(RE::ExtraDataList* a_list, RE::BGSOutfit* a_outfit)`;
// under the list's lock at +0x18 it finds the list's ExtraOutfitItem and
// writes the outfit's id into it, or allocates one (0x18 bytes) on
// VTABLE_ExtraOutfitItem with the id at +0x10 and adds it to the list. Taken
// over the hand-written insert this file carried, since every byte it
// touches is the fork's own list layout (CreateExtraList below) and the
// call's two arguments are confirmed. RE this: its return (called as void,
// the value unread).
using SetOutfitItemTagFn = void (*)(RE::ExtraDataList* list,
                                    RE::BGSOutfit* outfit);

// 16053, the entry adder 16155 and 16128 add through (re-npc-roll.md 8.2 and
// 8.5, HYPOTHESIS): `(RE::InventoryChanges* a_this, RE::TESForm* a_form,
// RE::ExtraDataList* a_list, std::int32_t a_count, RE::TESObjectREFR*
// a_fromRefr)`, a_fromRefr nullptr at this site, the list the entry's from
// here. It is the engine's own call inside the init; TESObjectREFR::
// AddObjectToContainer (vfunc 0x5A, include/RE/T/TESObjectREFR.h:286) is the
// CommonLib-named fallback but may raise container events mid-init (8.4,
// HYPOTHESIS). RE this: its return (called as void, the value unread).
using AddEntryWithListFn = void (*)(RE::InventoryChanges* changes,
                                    RE::TESForm* form, RE::ExtraDataList* list,
                                    std::int32_t count,
                                    RE::TESObjectREFR* fromRefr);

// A fresh, empty ExtraDataList the way this plugin's AddItemEx makes one for
// AddObjectToContainer (PapyrusTESModPlatform.cpp:597-624, the fork's
// idiom): CommonLibSSE-NG declares ExtraDataList() (include/RE/E/
// ExtraDataList.h:136) and the pin's sources do not define it (8.4's "if the
// pin defines it": it does not), and under ENABLE_SKYRIM_AE the type carries
// no lock member (:207-209: the lock sits at 0x18 on AE .629 and later,
// reached through GetLock(), :202), so the engine's size is the fork's
// constant: BaseExtraList's data and presence pointers (:18-38) and the
// lock, all zeroed (a zero BSReadWriteLock is unlocked, include/RE/B/
// BSAtomic.h:70), 0x20 in all, the engine's own Allocate size at this site
// (8.2). The presence bitfield (0x18 bytes, :21-28) is allocated and zeroed
// as the idiom does; the tag is the engine's to add (11647).
RE::ExtraDataList* CreateExtraList()
{
  constexpr size_t kBaseExtraListSizeMax = 24;
  constexpr size_t kSpinLockSizeMax = 8;
  constexpr size_t kExtraListSize = kBaseExtraListSizeMax + kSpinLockSizeMax;
  auto* memory = static_cast<uint8_t*>(RE::malloc(kExtraListSize));
  if (!memory) {
    return nullptr;
  }
  auto* presence = RE::malloc<RE::BaseExtraList::PresenceBitfield>();
  if (!presence) {
    // include/RE/M/MemoryManager.h:187
    RE::free(memory);
    return nullptr;
  }
  std::memset(memory, 0, kExtraListSize);
  std::memset(presence, 0, sizeof(*presence));
  auto* list = reinterpret_cast<RE::ExtraDataList*>(memory);
  list->_extraData.GetPresence() = presence;
  return list;
}

// The engine's own skip at this site (re-npc-roll.md 8.2: 16133 finds the
// owner's entry for the form, 15988 asks it for an ExtraOutfitItem, 15989
// reads that tag's id; a pick whose entry already carries this outfit's tag
// is not added again), done through CommonLibSSE-NG's names instead of those
// three HYPOTHESIS ids: InventoryChanges::entryList (include/RE/I/
// InventoryChanges.h:63), InventoryEntryData::object and extraLists
// (include/RE/I/InventoryEntryData.h:59-60), BSSimpleList's range iteration
// (include/RE/B/BSTList.h:9, :170, :225-228; the fork's shape at
// PapyrusTESModPlatform.cpp:1142-1144 and EventHandler.cpp:2345-2346),
// ExtraDataList::GetByType<T> (include/RE/E/ExtraDataList.h:152-155),
// ExtraOutfitItem::id (include/RE/E/ExtraOutfitItem.h:20). It is what keeps
// 16129's second call (a sleepOutfit that is the same form as the
// defaultOutfit) from dressing twice.
bool CarriesOutfitTag(RE::InventoryChanges* changes, RE::TESBoundObject* form,
                      RE::FormID outfitId)
{
  if (!changes->entryList) {
    return false;
  }
  for (auto* entry : *changes->entryList) {
    if (!entry || entry->object != form || !entry->extraLists) {
      continue;
    }
    for (auto* list : *entry->extraLists) {
      const auto* tag =
        list ? list->GetByType<RE::ExtraOutfitItem>() : nullptr;
      if (tag && tag->id == outfitId) {
        return true;
      }
    }
  }
  return false;
}

bool OutfitResolveReplacement(RE::BSTArray<RE::TESForm*>* items,
                              OutfitInitCtx* ctx)
{
  if (!Fates::Enabled() || !ctx || !ctx->outfit || !ctx->changes) {
    return g_outfitResolveOriginal(items, ctx);
  }
  // InventoryChanges::owner (include/RE/I/InventoryChanges.h:64); the outfit
  // must be the owner's base's defaultOutfit: TESForm::As<T> (include/RE/T/
  // TESForm.h:257), Actor::GetActorBase (include/RE/A/Actor.h:524, the base
  // object As<TESNPC>, src/RE/A/Actor.cpp:256-260), TESNPC::defaultOutfit
  // (include/RE/T/TESNPC.h:281). The sleepOutfit call (:282), a container
  // owner and an unfated actor fall through to the engine's roll; the outfit
  // test comes before the table's lock so the common case costs no lock.
  RE::TESObjectREFR* owner = ctx->changes->owner;
  auto* actor = owner ? owner->As<RE::Actor>() : nullptr;
  auto* npc = actor ? actor->GetActorBase() : nullptr;
  if (!npc || npc->defaultOutfit != ctx->outfit) {
    return g_outfitResolveOriginal(items, ctx);
  }
  Fates::Fate fate;
  // TESForm::formID (include/RE/T/TESForm.h:354)
  if (!Fates::Get(owner->formID, fate)) {
    return g_outfitResolveOriginal(items, ctx);
  }
  const auto dressed =
    Fates::AddWornTagged(ctx->changes, ctx->outfit, fate, owner->formID);
  logger::info("[verb:npc-fate] dressed {:x} from the server's roll inside "
               "the outfit roll: {} of {} tagged for outfit {:x} (npc level "
               "{}, {} already tagged, the engine's outfit roll skipped)",
               owner->formID, dressed.added, fate.worn.size(),
               ctx->outfit->formID, ctx->level, dressed.carried);
  return true;
}
}

Fates::Dressing Fates::AddWornTagged(RE::InventoryChanges* changes,
                                     RE::BGSOutfit* outfit, const Fate& fate,
                                     uint32_t ownerId)
{
  Dressing result;
  if (!changes || !outfit) {
    return result;
  }
  REL::Relocation<SetOutfitItemTagFn> setTag{
    Offsets::Fate::SetOutfitItemTag
  };
  REL::Relocation<AddEntryWithListFn> addEntry{
    Offsets::Fate::AddEntryWithList
  };
  for (const auto wornId : fate.worn) {
    // an inventory entry needs a bound object (InventoryEntryData::object,
    // include/RE/I/InventoryEntryData.h:59; AddObjectToContainer's first
    // argument, include/RE/T/TESObjectREFR.h:286); LookupByID<TESBoundObject>
    // is As<TESBoundObject> on the form (include/RE/T/TESForm.h:223-227)
    auto* bound = RE::TESForm::LookupByID<RE::TESBoundObject>(wornId);
    if (!bound) {
      logger::warn("[verb:npc-fate] worn item {:x} for {:x} is no bound "
                   "object in this game, skipped",
                   wornId, ownerId);
      continue;
    }
    if (CarriesOutfitTag(changes, bound, outfit->formID)) {
      ++result.carried;
      continue;
    }
    auto* list = CreateExtraList();
    if (!list) {
      logger::warn("[verb:npc-fate] no memory for {:x}'s outfit tag, the "
                   "rest of the list skipped",
                   ownerId);
      break;
    }
    // the engine's tag (11647), then the engine's add (16053): one entry,
    // count 1, no source reference; the list is the entry's from here
    setTag(list, outfit);
    addEntry(changes, bound, list, 1, nullptr);
    ++result.added;
  }
  return result;
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
    handler->Replace(Offsets::Hooks::OutfitResolve.address(),
                     reinterpret_cast<void*>(&OutfitResolveReplacement),
                     reinterpret_cast<void**>(&g_outfitResolveOriginal));
  logger::info("[verb:npc-fate] detours installed, inert until "
               "setActorFateEnabled(true): leveled roll {}, outfit resolve {}",
               roll, outfit);
}
