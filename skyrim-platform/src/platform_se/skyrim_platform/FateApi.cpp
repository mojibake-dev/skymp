#include "FateApi.h"
#include "CallNativeApi.h"
#include "Fates.h"
#include "ItemMenuRefresh.h"
#include "Puppets.h"
#include "WeaponAnimGuard.h"
#include "game/Offsets.h"
#include <RE/A/AIProcess.h>
#include <RE/A/Actor.h>
#include <RE/A/ActorState.h>
#include <RE/M/Main.h>
#include <RE/M/MiddleHighProcessData.h>
#include <RE/P/PlayerCharacter.h>
#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
// The engine's re-roll of a leveled actor, 37323 (docs/research/m3/
// re-npc-roll.md section 1, HYPOTHESIS): `(RE::Actor* a_actor, int a_arg)`;
// it restores the placed base, drops the clone and the 3D and re-resolves
// through 14374, hence through the leveled roll detour (FateHooks.cpp), then
// re-initializes the inventory and switches the race; a_arg == -1 skips its
// level check. RE this: its return type (called as void, the value unread).
using ActorRerollFn = void (*)(RE::Actor* actor, std::int32_t arg);
}

namespace {
// the inventory rows as the TS side passes them: one flat array of numbers,
// [formId, count, formId, count, ...]; absent or empty: no rows (an older
// bundle, a server whose fate carries none)
void ParseInvRows(const Napi::CallbackInfo& info, size_t index,
                  Fates::Fate& fate)
{
  if (info.Length() <= index || info[index].IsUndefined() ||
      info[index].IsNull()) {
    return;
  }
  const auto flat = NapiHelper::ExtractArray(info[index], "inv");
  const uint32_t n = flat.Length();
  fate.inv.reserve(n / 2);
  for (uint32_t i = 0; i + 1 < n; i += 2) {
    const std::string idName = "inv[" + std::to_string(i) + "]";
    const std::string countName = "inv[" + std::to_string(i + 1) + "]";
    const auto baseId = NapiHelper::ExtractUInt32(flat.Get(i), idName.c_str());
    const auto count =
      NapiHelper::ExtractUInt32(flat.Get(i + 1), countName.c_str());
    if (baseId == 0 || count == 0) {
      continue;
    }
    fate.inv.emplace_back(baseId, count);
  }
}
}

Napi::Value FateApi::SetActorFate(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  const auto baseId = NapiHelper::ExtractUInt32(info[1], "baseId");
  const auto worn = NapiHelper::ExtractArray(info[2], "worn");
  if (refrId == 0 || refrId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  Fates::Fate fate;
  fate.baseId = baseId;
  const uint32_t n = worn.Length();
  fate.worn.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string subArgName = "worn[" + std::to_string(i) + "]";
    fate.worn.push_back(
      NapiHelper::ExtractUInt32(worn.Get(i), subArgName.c_str()));
  }
  ParseInvRows(info, 3, fate);
  Fates::Set(refrId, fate);
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value FateApi::ArmActorFate(const Napi::CallbackInfo& info)
{
  const auto baseId = NapiHelper::ExtractUInt32(info[0], "baseId");
  const auto fateBaseId = NapiHelper::ExtractUInt32(info[1], "fateBaseId");
  const auto worn = NapiHelper::ExtractArray(info[2], "worn");
  if (baseId == 0) {
    return Napi::Boolean::New(info.Env(), false);
  }
  Fates::Fate fate;
  fate.baseId = fateBaseId;
  const uint32_t n = worn.Length();
  fate.worn.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string subArgName = "worn[" + std::to_string(i) + "]";
    fate.worn.push_back(
      NapiHelper::ExtractUInt32(worn.Get(i), subArgName.c_str()));
  }
  ParseInvRows(info, 3, fate);
  Fates::Arm(baseId, fate);
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value FateApi::ClearActorFate(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  return Napi::Boolean::New(info.Env(), Fates::Clear(refrId));
}

Napi::Value FateApi::SetActorFateEnabled(const Napi::CallbackInfo& info)
{
  const auto on = NapiHelper::ExtractBoolean(info[0], "on");
  Fates::SetEnabled(on);
  logger::info("[verb:npc-fate] imposition {} ({} fates held)",
               on ? "on" : "off", Fates::Count());
  return info.Env().Undefined();
}

namespace {
// thuum docs/verbs/npc-fate.md THE RAGDOLL CRASH READ (c1-crash-2026-11-04-
// 12-30-04 in x-fate-inventory 20261104-202729): the fate's engine work (a
// re-roll's Set3D and Load3D, a redress, a resurrect) runs on the game's MAIN
// thread through SKSE's UI task queue, the thread the engine does its own
// attach on, the shape change-form's apply has (ChangeForm.cpp, CONFIRMED
// there on the "thread {} main {}" line). On the Papyrus VM thread, where
// gameThrQ drains (main.cpp's OnUpdate), TESObjectREFR::Set3D (19730) asks
// TaskQueueInterface::ShouldUseTaskQueue (39033) and DEFERS the old 3D's
// DetachHavok (37307) to the main thread as task 0x32 (36933); a Load3D that
// follows on the VM thread binds the new skeleton (63596, 63792) while the
// main thread's drain reads the NEW 3D and pulls its collision objects from
// the world, and the ragdoll collector (78536) walks a constraints buffer
// freed under it (the re-analyst's read, HYPOTHESIS until
// lab/frida/reroll-main-trace.js reads the drain against the bind). Without
// the interface the work is refused, as the apply refuses (never the VM
// thread, the path this exists to remove); a throw inside the task is caught
// and said, since SKSE's UI drain has no net of its own (the review of
// cc5c82b6, findings 2 and 8).
bool QueueFateTask(const char* what, uint32_t refrId,
                   std::function<void()> task)
{
  auto* tasks = SKSE::GetTaskInterface();
  if (!tasks) {
    static bool said = false;
    if (!said) {
      said = true;
      logger::warn("[verb:npc-fate] {}({:x}): no SKSE task interface, the "
                   "fate's engine work is refused",
                   what, refrId);
    }
    return false;
  }
  tasks->AddUITask([what, refrId, task = std::move(task)]() {
    try {
      task();
    } catch (const std::exception& e) {
      logger::error("[verb:npc-fate] {}({:x}) threw on the main thread: {}",
                    what, refrId, e.what());
    }
  });
  return true;
}

// the thread a fate task ran on, beside Main's (Main.h:87 threadID), the
// witness the lab reads
std::string FateThreadNote()
{
  const auto* main = RE::Main::GetSingleton();
  const auto thread = static_cast<uint32_t>(::GetCurrentThreadId());
  const uint32_t mainThread = main ? main->threadID : 0u;
  return "thread " + std::to_string(thread) + " main " +
    std::to_string(mainThread);
}
}

namespace {
// a stack the engine wears: its list carries ExtraWorn or ExtraWornLeft
// (ExtraDataList::HasType, include/RE/E/ExtraDataList.h:163; InventoryEntry
// Data::IsWorn reads the same two, src/RE/I/InventoryEntryData.cpp:223-234)
bool IsWornList(const RE::ExtraDataList& list)
{
  return list.HasType(RE::ExtraDataType::kWorn) ||
    list.HasType(RE::ExtraDataType::kWornLeft);
}

// thuum docs/verbs/npc-fate.md (playtest 48, the loot strip; Eli: "when i
// activate the person their armor is unequipped in their inventory stripping
// them naked"): the record's worn armor a dead copy holds but does not wear
// is equipped again, never left loose in its inventory; run after the
// reconcile's removals and adds, and after the client's apply of the
// record's list (dressBody below). Armor alone (TESForm::IsArmor, include/RE/
// T/TESForm.h:323; a shield is ARMO): a weapon, the ammo and a torch are the
// hands', which the engine keeps on a corpse as it found them, and
// WornOutfitEquals leaves the weapons out the same way. ActorEquipManager::
// EquipObject (include/RE/A/ActorEquipManager.h:19) with the queue off, no
// sound and applied now, the reconcile's own unequip shape, so nothing is
// queued across the removals before it (the 18531 crash read below); the
// stack equipped is an unworn list of the piece when it has one, else the
// count no list carries (nullptr). HYPOTHESIS until x-body-open-worn reads
// the worn list kept on both seats: that EquipObject dresses a dead actor,
// its AI off, in place, and that a queued equip would wait on a process a
// corpse never runs. Returns the pieces equipped.
size_t DressBodyOnGameThread(RE::Actor* actor,
                             const std::vector<uint32_t>& worn)
{
  auto* equipManager = RE::ActorEquipManager::GetSingleton();
  if (!equipManager || worn.empty()) {
    return 0;
  }
  const std::set<uint32_t> wanted(worn.begin(), worn.end());
  size_t dressed = 0;
  auto inventory = actor->GetInventory(
    [](RE::TESBoundObject& o) { return !o.Is(RE::FormType::LeveledItem); });
  for (auto& [object, entry] : inventory) {
    if (!object || entry.first <= 0 || !object->IsArmor() ||
        wanted.count(object->formID) == 0) {
      continue;
    }
    bool wornHere = false;
    RE::ExtraDataList* unworn = nullptr;
    if (entry.second && entry.second->extraLists) {
      for (auto* list : *entry.second->extraLists) {
        if (!list) {
          continue;
        }
        if (IsWornList(*list)) {
          wornHere = true;
        } else if (!unworn) {
          unworn = list;
        }
      }
    }
    if (wornHere) {
      continue;
    }
    equipManager->EquipObject(actor, object, unworn, 1, nullptr, false, false,
                              false, true);
    ++dressed;
  }
  return dressed;
}
}

namespace {
// re-npc-roll.md 8.4's late path on the game thread: the actor looked up
// here (setPuppet's shape, the 2026-10-21 hang), the outfit stripped, the
// server's worn list added tagged as the detour adds it, worn again
// thuum docs/verbs/npc-fate.md (playtest 44's 1c, the longbow): a copy
// loaded before its fate carries its own engine's container roll (the Long
// Bow and arrows the record never had, in the loot window). The record's
// rows are the truth: every non-worn count is brought to the fate's rows and
// the worn set to the fate's worn list, by the engine's own adder and
// remover on the game thread (TESObjectREFR::AddObjectToContainer and
// RemoveItem, include/RE/T/TESObjectREFR.h:282-286; the counts read through
// GetInventory, :400). HYPOTHESIS until x-fate-inventory reads the window
// equal to the record on a late joiner.
void ReconcileInventoryOnGameThread(RE::Actor* actor, const Fates::Fate& fate,
                                    uint32_t refrId)
{
  std::unordered_map<uint32_t, int32_t> want;
  for (const auto& [baseId, count] : fate.inv) {
    want[baseId] += static_cast<int32_t>(count);
  }
  for (const auto wornId : fate.worn) {
    want[wornId] += 1;
  }
  size_t removed = 0;
  size_t added = 0;
  size_t kept = 0;
  size_t wornRemoved = 0;
  // the leveled-list bases the engine lists for a container with no rolled
  // entry yet are no items (the review of 7c3c9f79, finding 3): left out
  const auto items = [](RE::TESBoundObject& o) {
    return !o.Is(RE::FormType::LeveledItem);
  };
  const auto targetOf = [&](RE::FormID id) {
    const auto it = want.find(id);
    return it == want.end() ? 0 : it->second;
  };
  // thuum #62 (on 2026-11-15 every Temple run with npc-fate on crashed a game
  // after its relaunch, the drop's deploy included, 20261115-220337; with
  // npcFate off, 2 of 2 ran clean): the engine's own order for
  // taking stacks out of a loaded actor, Actor::RemoveOutfitItems (Address
  // Library 19690, CommonLibSSE-NG src/RE/A/Actor.cpp:1300-1305; its body by
  // REA on sky-re, Ghidra 12.1.4, 2026-11-15), the remover CommonLib's own
  // SetDefaultOutfit calls (:1016-1029): for each list it takes, one call to
  // the actor's RemoveItem (vtable +0x2B0, TESObjectREFR::RemoveItem's slot
  // 0x56, include/RE/T/TESObjectREFR.h:282) with that list and no
  // UnequipObject before it, worn or not, and after every removal it reads
  // the inventory changes again from the top, since a removal can free or
  // merge lists. This loop did otherwise since 802842d9 (2026-11-04): it
  // copied out every list of an item first, then for a worn one called
  // ActorEquipManager::UnequipObject applied now and then RemoveItem with the
  // same pointer, so a list the unequip or an earlier removal merged or freed
  // reached RemoveItem dangling (HYPOTHESIS: the heap write behind #62's
  // scattered crashes; the Temple's late reconciles strip worn stacks from
  // corpses and from Arvel's live copy in one millisecond, 20261115-132319 c1
  // 05:30:11.134). Now one stack a pass, read afresh each pass, by its own
  // list (the purge of a queued equip still matches it, the 06:12Z crashes'
  // rule): an unworn list first, then the count no list carries, then a
  // worn list, which RemoveItem unequips as the engine's remover relies on.
  // A removal that leaves an item's count where it was ends that item's
  // turn (its later passes skip it), and the passes are bounded
  constexpr int kRemovePassesMax = 512;
  std::set<RE::FormID> stuck;
  int passes = 0;
  for (; passes < kRemovePassesMax; ++passes) {
    RE::TESBoundObject* object = nullptr;
    RE::ExtraDataList* list = nullptr;
    int32_t count = 0;
    int32_t have = 0;
    bool worn = false;
    auto inventory = actor->GetInventory(items);
    for (auto& [candidate, entry] : inventory) {
      if (!candidate || entry.first <= 0 ||
          stuck.count(candidate->formID) != 0) {
        continue;
      }
      const int32_t over = entry.first - targetOf(candidate->formID);
      if (over <= 0) {
        continue;
      }
      RE::ExtraDataList* unworn = nullptr;
      RE::ExtraDataList* wornList = nullptr;
      int32_t inLists = 0;
      if (entry.second && entry.second->extraLists) {
        for (auto* l : *entry.second->extraLists) {
          if (!l) {
            continue;
          }
          inLists += std::max<int32_t>(1, l->GetCount());
          if (IsWornList(*l)) {
            if (!wornList) {
              wornList = l;
            }
          } else if (!unworn) {
            unworn = l;
          }
        }
      }
      const int32_t loose = std::max<int32_t>(0, entry.first - inLists);
      object = candidate;
      have = entry.first;
      if (unworn) {
        list = unworn;
        count = std::min(std::max<int32_t>(1, unworn->GetCount()), over);
      } else if (loose > 0) {
        list = nullptr;
        count = std::min(loose, over);
      } else if (wornList) {
        list = wornList;
        worn = true;
        count = std::min(std::max<int32_t>(1, wornList->GetCount()), over);
      } else {
        list = nullptr;
        count = over;
      }
      break;
    }
    if (!object) {
      break;
    }
    // TESObjectREFR::RemoveItem (include/RE/T/TESObjectREFR.h:282), the
    // handle it answers unread; `list` is the entry's own, read this pass
    (void)actor->RemoveItem(object, count, RE::ITEM_REMOVE_REASON::kRemove,
                            list, nullptr);
    const auto after = actor->GetInventoryCounts(items);
    const auto left = after.find(object);
    const int32_t now = left == after.end() ? 0 : left->second;
    if (now >= have) {
      stuck.insert(object->formID);
      logger::warn("[verb:npc-fate] inventory of {:x}: {} of {:x} not "
                   "removed (it holds {} still), left as it is",
                   refrId, count, object->formID, now);
      continue;
    }
    removed += static_cast<size_t>(have - now);
    if (worn) {
      ++wornRemoved;
    }
  }
  if (passes == kRemovePassesMax) {
    logger::warn("[verb:npc-fate] inventory of {:x}: removals stopped after "
                 "{} passes",
                 refrId, kRemovePassesMax);
  }
  // then the adds, from the counts as they stand: each row's missing count by
  // the engine's adder, TESObjectREFR::AddObjectToContainer
  // (include/RE/T/TESObjectREFR.h:286)
  const auto counts = actor->GetInventoryCounts(items);
  for (const auto& [object, have] : counts) {
    if (object && have > 0) {
      kept += static_cast<size_t>(std::min(have, targetOf(object->formID)));
    }
  }
  for (const auto& [baseId, target] : want) {
    auto* bound = RE::TESForm::LookupByID<RE::TESBoundObject>(baseId);
    if (!bound) {
      if (target > 0) {
        logger::warn("[verb:npc-fate] inventory row {:x} for {:x} is no "
                     "bound object in this game, skipped",
                     baseId, refrId);
      }
      continue;
    }
    const auto it = counts.find(bound);
    const int32_t have = it == counts.end() ? 0 : it->second;
    const int32_t missing = target - have;
    if (missing <= 0) {
      continue;
    }
    actor->AddObjectToContainer(bound, nullptr, missing, nullptr);
    added += static_cast<size_t>(missing);
  }
  // a corpse's worn armor is the record's (the loot strip, above); a living
  // copy's is its outfit's, which the redress dresses after this reconcile
  // (Actor::IsDead, include/RE/A/Actor.h)
  const size_t dressed =
    actor->IsDead() ? DressBodyOnGameThread(actor, fate.worn) : 0;
  // the thread it ran on (thuum #62: the Temple crashes came 150 to 380 ms
  // after a burst of these removals on live copies, on BSJobs threads
  // reading the same actors)
  logger::info("[verb:npc-fate] inventory of {:x} reconciled to the record: "
               "{} row(s) and {} worn, {} removed ({} worn stack(s) among "
               "them, by their own lists, {} pass(es)), {} added, {} kept, {} "
               "worn piece(s) dressed again, on {}",
               refrId, fate.inv.size(), fate.worn.size(), removed, wornRemoved,
               passes, added, kept, dressed, FateThreadNote());
}

// The copy's worn OUTFIT set against the fate's, as sets of base ids: the
// entries GetInventory lists as worn (InventoryEntryData::IsWorn, include/
// RE/I/InventoryEntryData.h:50; the leveled-list bases left out as in the
// reconcile), weapons left out on both sides (TESForm::IsWeapon, include/
// RE/T/TESForm.h:347): the redress is the outfit's, the fate's weapons are
// the container detour's rows and the AI equips them in its own time (a
// weapon not yet in hand three seconds after the load must not force a
// strip). Equal, the detours or an earlier redress dressed this copy
// already and the strip, the re-add and the dress have nothing to do.
bool WornOutfitEquals(RE::Actor* actor, const Fates::Fate& fate)
{
  std::set<uint32_t> want;
  for (const auto id : fate.worn) {
    auto* form = RE::TESForm::LookupByID(id);
    if (form && !form->IsWeapon()) {
      want.insert(id);
    }
  }
  std::set<uint32_t> have;
  auto inventory = actor->GetInventory(
    [](RE::TESBoundObject& o) { return !o.Is(RE::FormType::LeveledItem); });
  for (auto& [object, entry] : inventory) {
    if (object && !object->IsWeapon() && entry.first > 0 && entry.second &&
        entry.second->IsWorn()) {
      have.insert(object->formID);
    }
  }
  return have == want;
}

void RedressOnGameThread(uint32_t refrId, const Fates::Fate& fate)
{
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
  if (!actor) {
    logger::warn("[verb:npc-fate] redressActor({:x}): no such actor", refrId);
    return;
  }
  // thuum docs/verbs/npc-fate.md 1e and 1c: a corpse keeps its look (the
  // change-form record lays it, ADR-065) and its inventory is the record
  // (Actor::IsDead, include/RE/A/Actor.h); the stand-up dresses it later
  if (actor->IsDead()) {
    ReconcileInventoryOnGameThread(actor, fate, refrId);
    logger::info("[verb:npc-fate] redressActor({:x}): the copy is dead here, "
                 "a corpse keeps its look (no redress); its inventory "
                 "reconciled to the record",
                 refrId);
    return;
  }
  // Actor::GetActorBase (include/RE/A/Actor.h:524), TESNPC::defaultOutfit
  // (include/RE/T/TESNPC.h:281): no default outfit, nothing the engine
  // would dress by (the lab spawn's figures and puppets, re-npc-roll.md
  // 8.4)
  auto* npc = actor->GetActorBase();
  auto* outfit = npc ? npc->defaultOutfit : nullptr;
  if (!outfit) {
    logger::warn("[verb:npc-fate] redressActor({:x}): the base has no "
                 "default outfit, nothing to redress",
                 refrId);
    return;
  }
  // the changes first: TESObjectREFR::GetInventoryChanges (include/RE/T/
  // TESObjectREFR.h:405; src/RE/T/TESObjectREFR.cpp:414-426) initializes
  // the inventory when the reference holds none, and that init rolls
  // through the outfit detour, so it runs before the strip below and the
  // strip takes its entries with the rest
  auto* changes = actor->GetInventoryChanges();
  if (!changes) {
    logger::warn("[verb:npc-fate] redressActor({:x}): no inventory "
                 "changes",
                 refrId);
    return;
  }
  // thuum docs/verbs/npc-fate.md 1e (x-fate-dress-bleak v8 20261104-100832:
  // both seats crashed in the equip queue's flush, 11696 under EquipObject
  // under 39814, within a second of a redress of 383ec that the detours had
  // dressed three seconds before, its reconcile "0 removed, 0 added"): the
  // strip (Actor::RemoveOutfitItems, the engine's own removal of the tagged
  // worn entries), the re-add and the dress on an already dressed living
  // copy free and re-make the very lists the AI process's equip queue
  // holds, for nothing. The engine's dresser skips an entry carrying its tag
  // (re-npc-roll.md 8.2); this does the same by the record: the copy's worn
  // set equal to the fate's, no strip, no re-add, no dress, the inventory
  // reconciled alone (itself a no-op when it matches). Since every loaded
  // copy passes rerollActor once (b763f766), this is the common case at a
  // cell's load. HYPOTHESIS until v8 reads the Temple's load with no crash.
  if (WornOutfitEquals(actor, fate)) {
    ReconcileInventoryOnGameThread(actor, fate, refrId);
    logger::info("[verb:npc-fate] redressActor({:x}): the copy wears the "
                 "fate's outfit already (3D loaded {}), no redress; its "
                 "inventory reconciled to the record",
                 refrId, actor->Is3DLoaded());
    return;
  }
  // re-npc-roll.md 8.4's three calls, CommonLib's SetDefaultOutfit
  // sequence (src/RE/A/Actor.cpp:1016-1029) less the outfit change:
  // Actor::RemoveOutfitItems (include/RE/A/Actor.h:617, bound to 19690 at
  // src/RE/A/Actor.cpp:1300-1305) with the outfit, not nullptr, to spare
  // the sleep outfit's entries; the same adds as the detour's; Actor::
  // AddWornOutfit (include/RE/A/Actor.h:502, bound to 19692 at :1293-1298)
  // with a_forceUpdate true: one pop
  actor->RemoveOutfitItems(outfit);
  const auto dressed = Fates::AddWornTagged(changes, outfit, fate, refrId);
  // the record's rows before the dress and the dress without the equip
  // queue (Actor::AddWornOutfit's second argument reaches the queue flag
  // of every tagged entry's equip; the engine's own dresser passes 0): the
  // crash read above, SetDefaultOutfit's order, remove first and dress
  // last with nothing queued across a removal. HYPOTHESIS until three
  // logins into Bleak Falls on this dist pass where 18531 crashed thrice.
  ReconcileInventoryOnGameThread(actor, fate, refrId);
  actor->AddWornOutfit(outfit, false);
  logger::info("[verb:npc-fate] redressed {:x} from the server's roll: {} "
               "of {} tagged for outfit {:x} and worn ({} already tagged, {} "
               "worn weapon(s) held already, 3D loaded {}), the record's rows "
               "reconciled first, the dress unqueued, on {}",
               refrId, dressed.added, fate.worn.size(), outfit->formID,
               dressed.carried, dressed.held, actor->Is3DLoaded(),
               FateThreadNote());
}
}

namespace {
// The re-roll's decision for a copy the engine holds, on the game thread:
// rerollActor's task (a late fate on a loaded copy) and the stand-up's
// (resurrectActor, right after Actor::Resurrect) run the same body, so a
// respawn whose fate names another pick is re-rolled through the engine on
// the living copy too (x-fate-dress-bleak v6 20261104-082903 on canon-4: the
// late fate reached the copy while it was still dead, reconcile-only, and
// nothing re-asked after the stand-up, so the stood-up copy kept its old
// pick); a dead copy reconciles its inventory alone.
void RerollOnGameThread(RE::Actor* actor, const Fates::Fate& fate,
                        uint32_t refrId)
{
  // thuum docs/verbs/npc-fate.md 1e (playtest 44's khajiit: a dead clone
  // re-rolled through the engine toward a respawn's new pick came out
  // half-built, naked and headless) and 1c (the body's inventory is the
  // record): a corpse is never re-rolled nor redressed, it keeps its look
  // and its inventory is reconciled to the record alone; a stand-up
  // (resurrectActor) dresses the living copy from the fate itself
  if (actor->IsDead()) {
    ReconcileInventoryOnGameThread(actor, fate, refrId);
    logger::info("[verb:npc-fate] rerollActor({:x}): the copy is dead "
                 "here, a corpse keeps its look (no re-roll, no redress); "
                 "its inventory reconciled to the record",
                 refrId);
    return;
  }
  // the decision is the engine's own mark: a leveled pick is a clone that
  // carries ExtraLeveledCreature (ExtraDataList::HasType, include/RE/E/
  // ExtraDataList.h:163; kLeveledCreature, E/ExtraDataTypes.h:219). The
  // Papyrus GetBaseObject the client read before answers that extra's
  // originalBase (the placed NPC_, an esm id) for every clone, so the
  // re-roll branch was never taken (b-npc-respawn 120743: the respawn's
  // new pick stayed the old species on both seats)
  if (!actor->extraList.HasType(RE::ExtraDataType::kLeveledCreature)) {
    logger::info("[verb:npc-fate] rerollActor({:x}): not a leveled clone, "
                 "redress instead",
                 refrId);
    RedressOnGameThread(refrId, fate);
    return;
  }
  // thuum docs/research/m3/decisions-after-playtest-34.md D23 (playtest 34,
  // section 4): a seat that logs in inside a dungeon loads its leveled
  // clones from the save before the fates arrive, and the save kept the
  // roll the server pinned, so the loaded clone IS the fate's pick in the
  // common case; the engine's re-roll below would drop its 3D for nothing
  // (thirteen invisible NPCs on test 2). The clone's pick is the extra's
  // templateBase (CommonLibSSE-NG include/RE/E/ExtraLeveledCreature.h:24;
  // HYPOTHESIS for the field's meaning, the log prints both ids): equal,
  // redress alone
  const auto* leveled = actor->extraList.GetByType<RE::ExtraLeveledCreature>();
  const uint32_t pickedId =
    leveled && leveled->templateBase ? leveled->templateBase->formID : 0;
  if (pickedId != 0 && pickedId == fate.baseId) {
    logger::info("[verb:npc-fate] rerollActor({:x}): the loaded clone is "
                 "the fate's pick {:x} already (3D loaded {}), redress alone",
                 refrId, pickedId, actor->Is3DLoaded());
    RedressOnGameThread(refrId, fate);
    return;
  }
  const bool had3D = actor->Is3DLoaded();
  REL::Relocation<ActorRerollFn> reroll{ Offsets::Fate::ActorReroll };
  // thuum lab/.cache/drafts/reads-53/r-c3-temple-crash.md (playtest 51 break
  // 7, "arm once, not twice"): every live re-roll before the crash logged
  // "armed" twice, 37323's own inventory init through the container detour
  // and the ResetInventory below, the second one throwing the first one's
  // entries away and writing the fate's weapons again. The detour's arms of
  // this owner on this thread are witnessed across 37323 (Fates::
  // ArmWitness), and the reset runs only when 37323 armed nothing (its init
  // elsewhere, or none), the order as before. HYPOTHESIS until a run of
  // this branch's build logs each re-roll "armed 1 time(s) by 37323's own
  // init, the inventory reset false" and dump-state reads its copy in the
  // fate's worn set (x-anim-listener runs canon, without this branch).
  std::uint32_t armedBy37323 = 0;
  {
    Fates::ArmWitness witness(refrId);
    reroll(actor, -1);
    armedBy37323 = witness.Arms();
  }
  // thuum docs/verbs/npc-fate.md 1e, H1 (playtest 44's 39fe1, the host's own
  // live bandit naked and headless after "rerolled ... from pick 37c06
  // toward 9a549"; Eli: "fix the living case before 45"). The engine's
  // attach caller pairs 37323 with ResetInventory and loads the 3D whole
  // afterwards (re-npc-roll.md section 9); on a LOADED copy 37323 drops the
  // 3D and Load3D(false) built the base model alone, no biped pass, no
  // facegen head. The engine's own rebuild for a living actor with a
  // process instead: the inventory reset (TESObjectREFR::ResetInventory,
  // include/RE/T/TESObjectREFR.h:338, Actor's override A/Actor.h:335; its
  // init rolls through the detours, which dress the fate's worn), the
  // record's dress and rows (RedressOnGameThread), then Actor::DoReset3D
  // (A/Actor.h:520), which flags the process's model, skin, head and face
  // passes (RESET_3D_FLAGS, M/MiddleHighProcessData.h:39-50) for the next
  // update; the race is the resolver's own SwitchRace (section 1, step 6).
  // HYPOTHESIS until x-fate-dress-bleak v7 reads the swapped copy dressed,
  // its race the fate's and its facegen node present on both seats.
  const bool reset = armedBy37323 == 0;
  if (reset) {
    actor->ResetInventory(false);
  }
  // oracle v4 20261104-184338 on cb944530: the redress ran while the model
  // was gone ("3D loaded FALSE" in its line) and the reload's biped init 28
  // ms later wore its own set (four of the fate's five, the cuirass missing),
  // while a respawn's re-roll with the same order happened to stick. The
  // order is the late path's own now: the model back FIRST through the
  // reference's loader (Actor::Load3D, A/Actor.h:315; 37323 drops the 3D
  // and nothing reloads it on an attached actor, D23), THEN the record's
  // dress and rows on the loaded model (RedressOnGameThread, idempotent
  // when the reset's own init dressed it through the detours), THEN the
  // skin, head and face passes flagged on the process and run at once
  // (AIProcess::Set3DUpdateFlag, A/AIProcess.h:188; RESET_3D_FLAGS,
  // M/MiddleHighProcessData.h:39-50; Actor::Update3DModel, A/Actor.h:633),
  // the head's own fix of cb944530 (dress v8 183635, oracle v4 184338: the
  // facegen node present after a respawn's and a login's re-roll). No
  // DoReset3D: on a model-less actor it only parked flags the reload
  // consumed (v8 104705's invisible copy was the drop with no reload).
  // HYPOTHESIS until oracle v4 reads the fate's worn beside faceNode true.
  bool reloaded = false;
  bool updated = false;
  if (had3D && !actor->Is3DLoaded()) {
    actor->Load3D(false);
    reloaded = true;
  }
  RedressOnGameThread(refrId, fate);
  if (had3D && actor->Is3DLoaded()) {
    if (auto* process = actor->GetActorRuntimeData().currentProcess) {
      process->Set3DUpdateFlag(RE::RESET_3D_FLAGS::kSkin);
      process->Set3DUpdateFlag(RE::RESET_3D_FLAGS::kHead);
      process->Set3DUpdateFlag(RE::RESET_3D_FLAGS::kFace);
      actor->Update3DModel();
      updated = true;
    }
  }
  logger::info("[verb:npc-fate] rerolled {:x} through the engine from pick "
               "{:x} toward the fate's {:x} (3D loaded {} before, {} after; "
               "armed {} time(s) by 37323's own init, the inventory reset {}, "
               "the model reloaded {}, the record dressed on it, the skin, "
               "head and face passes run {}) on {}",
               refrId, pickedId, fate.baseId, had3D, actor->Is3DLoaded(),
               armedBy37323, reset, reloaded, updated, FateThreadNote());
}

void RerollTask(uint32_t refrId, uint32_t tick);

// x-anim-listener 20261115-051655 (WeaponAnimGuard.h): the re-roll's one
// entry for both of its callers (rerollActor's task and the stand-up's),
// every branch of it a dress (the dead copy's reconcile and its
// DressBodyOnGameThread, the redress's strip and AddWornOutfit, 37323's
// re-roll and its rebuild), so every branch passes the guard: a dead copy
// whose record's apply is queued or running is held for that apply (the
// lay dresses it; 051655's 0x375aa was dressed here 43 ms before its
// apply's finish pass), and no branch starts while a weapon-animation
// request is outstanding on the actor (a second dress while one waits,
// which 39295 answers by releasing the waiting request's listener
// uncancelled when the weapons differ and the loaded set is not empty).
// Deferred to a later tick, bounded; the deferred or held run reads the
// fate and the actor again. A request the pass itself starts and meets
// later inside the same task (37323's reload, then the redress) is the
// pass's own, the engine's attach order (a known limit).
WeaponAnimGuard::Decision RerollWhenIdle(RE::Actor* actor,
                                         const Fates::Fate& fate,
                                         uint32_t refrId, uint32_t tick)
{
  const auto decision = WeaponAnimGuard::Check(
    WeaponAnimGuard::Kind::kReroll, actor, refrId, tick,
    [refrId](uint32_t next) {
      if (!QueueFateTask("rerollActor", refrId,
                         [refrId, next]() { RerollTask(refrId, next); })) {
        WeaponAnimGuard::Forget(WeaponAnimGuard::Kind::kReroll, refrId);
      }
    },
    [refrId]() { RerollTask(refrId, 0); });
  if (decision == WeaponAnimGuard::Decision::kRun) {
    RerollOnGameThread(actor, fate, refrId);
  }
  return decision;
}

// a deferred re-roll on its later tick, or a held one (tick 0) after its
// apply answered refused, on the main thread: the newest fate the server
// sent and the actor as it is now
void RerollTask(uint32_t refrId, uint32_t tick)
{
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
  Fates::Fate fate;
  if (!actor || !Fates::Enabled() || !Fates::Get(refrId, fate)) {
    WeaponAnimGuard::Forget(WeaponAnimGuard::Kind::kReroll, refrId);
    logger::info("[verb:npc-fate] rerollActor({:x}): {}, then {}: nothing "
                 "re-rolled",
                 refrId,
                 tick > 0 ? fmt::format("deferred {} tick(s)", tick)
                          : std::string("held for a refused apply"),
                 actor ? "its fate is gone or the switch is off"
                       : "no such actor");
    return;
  }
  (void)RerollWhenIdle(actor, fate, refrId, tick);
}

// redressActor's pass on the main thread, tick 0 from the native and the
// deferral's count after (a held one runs at 0 again): the guard first, as
// the re-roll's, then the newest fate's redress (RedressOnGameThread looks
// the actor up again, as it always has)
void RedressTask(uint32_t refrId, uint32_t tick)
{
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
  Fates::Fate fate;
  if (!actor || !Fates::Enabled() || !Fates::Get(refrId, fate)) {
    WeaponAnimGuard::Forget(WeaponAnimGuard::Kind::kRedress, refrId);
    logger::warn("[verb:npc-fate] redressActor({:x}): {}", refrId,
                 actor ? "its fate is gone or the switch is off, nothing "
                         "redressed"
                       : "no such actor");
    return;
  }
  const auto decision = WeaponAnimGuard::Check(
    WeaponAnimGuard::Kind::kRedress, actor, refrId, tick,
    [refrId](uint32_t next) {
      if (!QueueFateTask("redressActor", refrId,
                         [refrId, next]() { RedressTask(refrId, next); })) {
        WeaponAnimGuard::Forget(WeaponAnimGuard::Kind::kRedress, refrId);
      }
    },
    [refrId]() { RedressTask(refrId, 0); });
  if (decision == WeaponAnimGuard::Decision::kRun) {
    RedressOnGameThread(refrId, fate);
  }
}

const char* DecisionName(WeaponAnimGuard::Decision d)
{
  switch (d) {
    case WeaponAnimGuard::Decision::kRun:
      return "run";
    case WeaponAnimGuard::Decision::kDeferred:
      return "deferred";
    case WeaponAnimGuard::Decision::kFolded:
      return "folded";
    case WeaponAnimGuard::Decision::kDropped:
      return "dropped";
    case WeaponAnimGuard::Decision::kHeld:
      return "held for the record's apply";
    default:
      return "unknown";
  }
}
}

Napi::Value FateApi::RerollActor(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  Fates::Fate fate;
  if (refrId == 0 || refrId == 0x14 || !Fates::Enabled() ||
      !Fates::Get(refrId, fate)) {
    return Napi::Boolean::New(info.Env(), false);
  }
  // the actor is looked up on the game thread alone (setPuppet's shape,
  // PuppetApi.cpp: a lookup on the JS thread runs under the forms map's lock
  // while the game thread waits for this tick, and a cell load holding that
  // lock hung a landing, 2026-10-21), where the re-roll has to run anyway:
  // it drops the 3D and re-initializes the inventory
  const bool queued = QueueFateTask("rerollActor", refrId, [refrId, fate]() {
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
    if (!actor) {
      logger::warn("[verb:npc-fate] rerollActor({:x}): no such actor", refrId);
      return;
    }
    (void)RerollWhenIdle(actor, fate, refrId, 0);
  });
  return Napi::Boolean::New(info.Env(), queued);
}

Napi::Value FateApi::RedressActor(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  Fates::Fate fate;
  if (refrId == 0 || refrId == 0x14 || !Fates::Enabled() ||
      !Fates::Get(refrId, fate)) {
    return Napi::Boolean::New(info.Env(), false);
  }
  // the actor is looked up on the game thread alone (rerollActor's and
  // setPuppet's shape, the 2026-10-21 hang), where the three engine calls
  // have to run anyway; the task reads the newest fate there, past the
  // guard (RedressTask)
  const bool queued = QueueFateTask("redressActor", refrId,
                                    [refrId]() { RedressTask(refrId, 0); });
  return Napi::Boolean::New(info.Env(), queued);
}

namespace {
// the newest worn list dressBody was given for each body still to dress: on
// the main thread alone (dressBody's tasks, the guard's later ticks and a
// refused apply's held passes all run there), so a dress the guard folds,
// defers or holds dresses the list its last ask gave
std::unordered_map<uint32_t, std::vector<uint32_t>>& BodyWorn()
{
  static std::unordered_map<uint32_t, std::vector<uint32_t>> worn;
  return worn;
}

// dressBody's pass on the main thread, tick 0 for an ask and the deferral's
// count after: the guard first (WeaponAnimGuard.h), then the record's worn
// armor the body holds unworn equipped again
void DressBodyTask(uint32_t refrId, uint32_t tick)
{
  auto& slots = BodyWorn();
  const auto slot = slots.find(refrId);
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
  if (slot == slots.end() || !actor || !actor->IsDead()) {
    WeaponAnimGuard::Forget(WeaponAnimGuard::Kind::kBody, refrId);
    if (slot != slots.end()) {
      slots.erase(slot);
    }
    if (!actor) {
      logger::warn("[verb:npc-fate] dressBody({:x}): no such actor", refrId);
    } else if (!actor->IsDead()) {
      // a living copy's worn set is its outfit's (redressActor), never this
      logger::info("[verb:npc-fate] dressBody({:x}): the copy is alive "
                   "here, nothing dressed",
                   refrId);
    }
    return;
  }
  const auto decision = WeaponAnimGuard::Check(
    WeaponAnimGuard::Kind::kBody, actor, refrId, tick,
    [refrId](uint32_t next) {
      if (!QueueFateTask("dressBody", refrId,
                         [refrId, next]() { DressBodyTask(refrId, next); })) {
        WeaponAnimGuard::Forget(WeaponAnimGuard::Kind::kBody, refrId);
      }
    },
    [refrId, worn = slot->second]() {
      // a refused apply hands it back with its list; the held passes run
      // in their order right after the answer, so the newest list held is
      // the last one written
      BodyWorn()[refrId] = worn;
      DressBodyTask(refrId, 0);
    });
  if (decision == WeaponAnimGuard::Decision::kDeferred ||
      decision == WeaponAnimGuard::Decision::kFolded) {
    // the list stays for the dress that waits
    return;
  }
  const auto worn = std::move(slot->second);
  slots.erase(slot);
  if (decision != WeaponAnimGuard::Decision::kRun) {
    return;
  }
  const size_t dressed = DressBodyOnGameThread(actor, worn);
  // an open item menu on the body hears it as AddItemEx's changes do
  if (dressed > 0) {
    (void)ItemMenuRefresh::TryPost(*actor);
  }
  logger::info("[verb:npc-fate] body {:x}: {} worn piece(s) of the "
               "record's {} dressed again after the client's apply, on {}",
               refrId, dressed, worn.size(), FateThreadNote());
}
}

Napi::Value FateApi::DressBody(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  const auto wornArg = NapiHelper::ExtractArray(info[1], "worn");
  if (refrId == 0 || refrId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  std::vector<uint32_t> worn;
  const uint32_t n = wornArg.Length();
  worn.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string subArgName = "worn[" + std::to_string(i) + "]";
    worn.push_back(
      NapiHelper::ExtractUInt32(wornArg.Get(i), subArgName.c_str()));
  }
  // thuum docs/verbs/npc-fate.md (playtest 48, the loot strip): after the
  // inventory changes the client's apply queued in this tick, then on the
  // main thread as the fate's other engine work. The apply's addItemEx tasks
  // sit on the game thread's queue, drained in order right after the tick
  // (main.cpp's OnUpdate; Viet::TaskQueue runs its tasks first in, first
  // out), so this task, queued behind them, hands the dress to SKSE's UI
  // queue only once they have landed; a UI task queued from the tick itself
  // could run on the main thread before them
  g_nativeCallRequirements.gameThrQ->AddTask([refrId, worn](Viet::Void) {
    (void)QueueFateTask("dressBody", refrId, [refrId, worn]() {
      // the newest list wins: a dress still waiting reads it at its run
      BodyWorn()[refrId] = worn;
      DressBodyTask(refrId, 0);
    });
  });
  return Napi::Boolean::New(info.Env(), true);
}

namespace {
// the stand-up on the main thread; tick 0 for resurrectActor's own task, the
// deferral's count for a later one (WeaponAnimGuard). `simulatedHere` is the
// client's word: false when another seat simulates the actor (thuum #48)
void ResurrectOnGameThread(uint32_t refrId, uint32_t tick, bool simulatedHere)
{
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
  if (!actor) {
    if (tick > 0) {
      WeaponAnimGuard::Forget(WeaponAnimGuard::Kind::kResurrect, refrId);
    }
    logger::warn("[verb:npc-fate] resurrectActor({:x}): no such actor",
                 refrId);
    return;
  }
  // thuum lab/.cache/drafts/reads-53/r-c3-temple-crash.md (playtest 51 break
  // 7, candidate A, HYPOTHESIS until x-anim-listener's trace): Resurrect's
  // inventory reset re-arms the copy through the container detour and its
  // attach re-equips, so the stand-up waits out a weapon-animation request
  // the engine still holds for the actor, as the re-roll does
  const auto decision = WeaponAnimGuard::Check(
    WeaponAnimGuard::Kind::kResurrect, actor, refrId, tick,
    [refrId, simulatedHere](uint32_t next) {
      if (!QueueFateTask("resurrectActor", refrId,
                         [refrId, next, simulatedHere]() {
                           ResurrectOnGameThread(refrId, next, simulatedHere);
                         })) {
        WeaponAnimGuard::Forget(WeaponAnimGuard::Kind::kResurrect, refrId);
      }
    });
  if (decision != WeaponAnimGuard::Decision::kRun) {
    return;
  }
  const bool puppet = Puppets::Is(refrId);
  const bool wasDead = actor->IsDead();
  // Actor::EnableAI (A/Actor.h:521), Actor::IsAIEnabled (A/Actor.h:579),
  // Actor::Resurrect (A/Actor.h:369), TESObjectREFR::SetActivationBlocked
  // (T/TESObjectREFR.h:464)
  // the AI on for the call whenever it is off, flagged or not: under AI off
  // the engine's Resurrect does not take (x-npc-respawn 070508), and since
  // playtest 37's findings 58 and 62 a dead body is released from the
  // puppet state on every seat (puppetService, through Promote's dead
  // branch: the block lifted, the AI left off, the flag cleared), so a
  // watcher's body arrives here unflagged with its AI off, as the host's
  // dead-promoted body did (H-54e)
  const bool aiOff = !actor->IsAIEnabled();
  if (aiOff) {
    actor->EnableAI(true);
  }
  actor->Resurrect(true, true);
  // thuum #48 (x-reanimate-claim v2.4 20261116-015143, step 59): a body
  // another seat's raise stood up (c2 commanding it) came up here with its
  // AI on (puppet false: a dead body leaves the puppet state on every seat)
  // and ran its own AI for 2 s, until PuppetService's next pass demoted it
  // (c1 17:56:14.607 to 16.611). A seat that does not simulate the actor
  // never simulates it, not for a frame (ADR-063): it comes up as the
  // puppet it is, the AI off and its activation blocked, and the pass then
  // flags it as before
  const bool keptPuppet = !puppet && !simulatedHere && !actor->IsDead();
  if (puppet || keptPuppet) {
    actor->EnableAI(false);
    actor->SetActivationBlocked(true);
  } else if (actor->IsDead()) {
    // the resurrection did not take: the body keeps H-54e's AI off, so
    // no package walks it (Hooks.cpp HostedResurrect's shape), and keeps
    // its block (ADR-067 decision 6): the puppet service's dead mark still
    // holds it, and an unblocked body under that mark would open the
    // engine's loot menu (the review of cc5c82b6, finding 3)
    if (aiOff) {
      actor->EnableAI(false);
    }
  } else {
    // a stood-up actor keeps the AI on, and a watcher's next pass demotes
    // it; its block from the kill comes off
    actor->SetActivationBlocked(false);
  }
  // thuum docs/verbs/npc-fate.md 1e (x-fate-dress-bleak v5 20261104-070209,
  // c1-relaunch-080: "redressed 39fe4" at :534 and this stand-up in the
  // same millisecond, Resurrect(true, true)'s inventory reset re-dressing
  // the engine's own outfit over the redress): the record's dress follows
  // the reset, never before it, so the stood-up copy wears the fate (the
  // pick's swap, if any, is the late fate's rerollActor after this)
  Fates::Fate fate;
  bool fated = false;
  const char* rerolled = "not asked";
  if (!actor->IsDead() && Fates::Enabled() && Fates::Get(refrId, fate)) {
    // the whole decision, not the redress alone: a respawn's fate that
    // names another pick re-rolls the stood-up clone through the engine
    // after the engine's reset. The reset's own equips may have left a
    // weapon-animation request outstanding, and a dress beside it is the
    // second dress WeaponAnimGuard.h names, so the decision passes the
    // guard: in this task when nothing is outstanding, else on the tick
    // after the request completes, still after the reset
    rerolled = DecisionName(RerollWhenIdle(actor, fate, refrId, 0));
    fated = true;
  }
  logger::info("[verb:npc-fate] resurrected {:x} (puppet {}, simulated here "
               "{}, kept a puppet {}, AI was off {}, dead before {}, dead "
               "after {}, dressed from the fate {}, the re-roll {}) on {}",
               refrId, puppet, simulatedHere, keptPuppet, aiOff, wasDead,
               actor->IsDead(), fated, rerolled, FateThreadNote());
}
}

Napi::Value FateApi::ResurrectActor(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  if (refrId == 0 || refrId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  // thuum #48: the second argument, true when absent (an older client)
  const bool simulatedHere = info.Length() < 2 || info[1].IsUndefined() ||
    info[1].IsNull() || NapiHelper::ExtractBoolean(info[1], "simulatedHere");
  const bool queued =
    QueueFateTask("resurrectActor", refrId, [refrId, simulatedHere]() {
      ResurrectOnGameThread(refrId, 0, simulatedHere);
    });
  return Napi::Boolean::New(info.Env(), queued);
}

Napi::Value FateApi::ActorFateCount(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(info.Env(), static_cast<double>(Fates::Count()));
}

namespace {
// thuum docs/verbs/npc-fate.md C4: the engine's command routines, by the
// static read's signatures (game/Offsets.h's Fate block cites each)
using CommandLifeStateFn = void (*)(RE::Actor* actor, std::uint8_t reanimate);
using CommandLinkFn = void (*)(RE::Actor* commanded, RE::Actor* commander,
                               RE::ActiveEffect* effect);
using CommandUnlinkFn = void (*)(RE::Actor* commanded,
                                 RE::ActiveEffect* effect);
// 38799 has the link's shape (Offsets.h)
using CommandRefreshFn = CommandLinkFn;

// The player's commandedActors entry for `thrall`
// (MiddleHighProcessData.h:147, CommandedActorData at :52-60): none, one
// holding no effect (this export's link), or one holding the engine's own
// command effect (the original cast's CommandEffect)
enum class CommandEntry
{
  kNone,
  kOurs,
  kEngine
};

CommandEntry CommandEntryOf(RE::Actor* player, RE::Actor* thrall)
{
  auto* process = player->GetActorRuntimeData().currentProcess;
  if (!process || !process->middleHigh) {
    return CommandEntry::kNone;
  }
  for (const auto& entry : process->middleHigh->commandedActors) {
    if (entry.commandedActor.get().get() == thrall) {
      return entry.activeEffect ? CommandEntry::kEngine : CommandEntry::kOurs;
    }
  }
  return CommandEntry::kNone;
}

// What this export did to each thrall, read and written on the main thread
// alone (every use below runs inside a QueueFateTask): linked, or evicted by
// the engine after the link (never linked again); once-lines said
enum class CommandMark
{
  kLinked,
  kEvicted
};
std::unordered_map<uint32_t, CommandMark> g_commandMarks;
std::unordered_map<uint32_t, bool> g_commandSaidEngine;
std::unordered_map<uint32_t, bool> g_commandSaidWait;
std::unordered_map<uint32_t, bool> g_commandSaidNoEntry;

void CommandOnGameThread(uint32_t refrId, bool on)
{
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
  auto* player = RE::PlayerCharacter::GetSingleton();
  if (!actor || !player) {
    logger::warn("[verb:npc-fate] commandActor({:x}): no such actor", refrId);
    return;
  }
  const CommandEntry entry = CommandEntryOf(player, actor);
  const auto mark = g_commandMarks.find(refrId);
  const bool dead = actor->IsDead();
  if (!on || dead) {
    if (mark == g_commandMarks.end()) {
      return;
    }
    const bool linked = mark->second == CommandMark::kLinked;
    g_commandMarks.erase(mark);
    g_commandSaidWait.erase(refrId);
    if (linked && entry == CommandEntry::kOurs) {
      REL::Relocation<CommandUnlinkFn> unlink{ Offsets::Fate::CommandUnlink };
      unlink(actor, nullptr);
      logger::info("[verb:npc-fate] commandActor({:x}): unlinked (38800; "
                   "{}), the player's entry {} after it, on {}",
                   refrId, dead ? "the thrall dead" : "no longer commanded",
                   CommandEntryOf(player, actor) == CommandEntry::kNone
                     ? "gone"
                     : "still there",
                   FateThreadNote());
    }
    return;
  }
  if (entry == CommandEntry::kEngine) {
    if (g_commandSaidEngine.emplace(refrId, true).second) {
      logger::info("[verb:npc-fate] commandActor({:x}): the engine's own "
                   "command, left alone",
                   refrId);
    }
    return;
  }
  if (mark != g_commandMarks.end() && mark->second == CommandMark::kEvicted) {
    return;
  }
  if (mark != g_commandMarks.end()) {
    if (entry == CommandEntry::kNone) {
      // 40056's eviction (the static read's G2): the entry went without
      // this export's 38800, so the engine dropped it for another; never
      // linked again here (H-C4-4)
      mark->second = CommandMark::kEvicted;
      logger::info("[verb:npc-fate] commandActor({:x}): the engine removed "
                   "it from the player's commanded actors; not linked again",
                   refrId);
      return;
    }
    REL::Relocation<CommandRefreshFn> refresh{ Offsets::Fate::CommandRefresh };
    refresh(actor, player, nullptr);
    return;
  }
  // the promotion first: a puppet (AI off, Puppets.h) is the record's copy
  // until the server's word turns it into this game's actor; asked again at
  // the client's next call (H-C4-5)
  const bool puppet = Puppets::Is(refrId);
  const bool aiOn = actor->IsAIEnabled();
  if (puppet || !aiOn) {
    if (g_commandSaidWait.emplace(refrId, true).second) {
      logger::info("[verb:npc-fate] commandActor({:x}): waits for its "
                   "promotion (puppet {}, AI on {})",
                   refrId, puppet, aiOn);
    }
    return;
  }
  const auto* state = actor->AsActorState();
  const bool reanimated =
    state && state->GetLifeState() == RE::ACTOR_LIFE_STATE::kReanimate;
  if (!reanimated) {
    REL::Relocation<CommandLifeStateFn> lifeState{
      Offsets::Fate::CommandLifeState
    };
    lifeState(actor, 1);
  }
  REL::Relocation<CommandLinkFn> link{ Offsets::Fate::CommandLink };
  link(actor, player, nullptr);
  const CommandEntry after = CommandEntryOf(player, actor);
  if (after != CommandEntry::kOurs) {
    // 38798 refused (its own tests: the commanded's process, a commander,
    // 37965; the static read's section 9) or the engine's entry is there:
    // no mark, so the next call is no eviction and asks again
    if (g_commandSaidNoEntry.emplace(refrId, true).second) {
      logger::info("[verb:npc-fate] commandActor({:x}): 38798 left no entry; "
                   "asked again (the player's entry {}) on {}",
                   refrId,
                   after == CommandEntry::kEngine ? "the engine's" : "missing",
                   FateThreadNote());
    }
    return;
  }
  g_commandMarks[refrId] = CommandMark::kLinked;
  g_commandSaidWait.erase(refrId);
  g_commandSaidNoEntry.erase(refrId);
  logger::info("[verb:npc-fate] commandActor({:x}): linked to the player "
               "({}38798; the player's entry with no effect, commanded {}) "
               "on {}",
               refrId, reanimated ? "" : "37490, ", actor->IsCommandedActor(),
               FateThreadNote());
}
}

Napi::Value FateApi::CommandActor(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  const auto on = NapiHelper::ExtractBoolean(info[1], "on");
  if (refrId == 0 || refrId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  const bool queued = QueueFateTask("commandActor", refrId, [refrId, on]() {
    CommandOnGameThread(refrId, on);
  });
  return Napi::Boolean::New(info.Env(), queued);
}
