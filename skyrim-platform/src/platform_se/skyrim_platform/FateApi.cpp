#include "FateApi.h"
#include "CallNativeApi.h"
#include "Fates.h"
#include "Puppets.h"
#include "game/Offsets.h"
#include <algorithm>
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
  size_t unequipped = 0;
  // the leveled-list bases the engine lists for a container with no rolled
  // entry yet are no items (the review of 7c3c9f79, finding 3): left out
  auto inventory = actor->GetInventory(
    [](RE::TESBoundObject& o) { return !o.Is(RE::FormType::LeveledItem); });
  for (auto& [object, entry] : inventory) {
    if (!object || entry.first <= 0) {
      continue;
    }
    const int32_t have = entry.first;
    const auto it = want.find(object->formID);
    const int32_t target = it == want.end() ? 0 : it->second;
    if (have > target) {
      // thuum docs/verbs/npc-fate.md 1c, the 06:12Z crashes on 18531 (the
      // re-analyst's static read of the three stacks, 2026-11-04,
      // HYPOTHESIS until the Frida trace): Actor::RemoveItem of a weapon
      // queues the next-best weapon's equip on the process (MiddleHigh
      // ProcessData::itemstoEquipUnequip, include/RE/M/
      // MiddleHighProcessData.h:157) with that entry's ExtraDataList, and
      // purges queued nodes only where the node's list is the a_extraList
      // passed (nullptr matched none of them); this loop then removed the
      // queued weapon too, freed its list, and the next actor-update job
      // on a job thread read it (39814 to EquipObject/UnequipObject, three
      // of three). The engine's own order instead: a worn stack is
      // unequipped first with the queue off and applied now (ActorEquip
      // Manager::UnequipObject, include/RE/A/ActorEquipManager.h:22), and
      // every stack is removed BY ITS OWN LIST (InventoryEntryData::
      // extraLists, include/RE/I/InventoryEntryData.h:61; ExtraDataList::
      // GetCount and HasType, include/RE/E/ExtraDataList.h) so the purge
      // matches; the listless remainder goes with nullptr as before. The
      // lists are copied out first: a removal frees the list it is given.
      int32_t toRemove = have - target;
      std::vector<RE::ExtraDataList*> lists;
      if (entry.second && entry.second->extraLists) {
        for (auto* list : *entry.second->extraLists) {
          if (list) {
            lists.push_back(list);
          }
        }
      }
      auto* equipManager = RE::ActorEquipManager::GetSingleton();
      for (auto* list : lists) {
        if (toRemove <= 0) {
          break;
        }
        const int32_t inList = std::max<int32_t>(1, list->GetCount());
        const int32_t n = std::min(inList, toRemove);
        if (equipManager &&
            (list->HasType(RE::ExtraDataType::kWorn) ||
             list->HasType(RE::ExtraDataType::kWornLeft))) {
          equipManager->UnequipObject(actor, object, list,
                                      static_cast<std::uint32_t>(n), nullptr,
                                      false, false, false, true);
          ++unequipped;
        }
        // TESObjectREFR::RemoveItem (include/RE/T/TESObjectREFR.h:282),
        // the handle it answers unread
        (void)actor->RemoveItem(object, n, RE::ITEM_REMOVE_REASON::kRemove,
                                list, nullptr);
        toRemove -= n;
      }
      if (toRemove > 0) {
        (void)actor->RemoveItem(
          object, toRemove, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
      }
      removed += static_cast<size_t>(have - target);
    } else {
      kept += static_cast<size_t>(have);
    }
    if (it != want.end()) {
      it->second = target - std::min(have, target);
    }
  }
  for (const auto& [baseId, missing] : want) {
    if (missing <= 0) {
      continue;
    }
    auto* bound = RE::TESForm::LookupByID<RE::TESBoundObject>(baseId);
    if (!bound) {
      logger::warn("[verb:npc-fate] inventory row {:x} for {:x} is no bound "
                   "object in this game, skipped",
                   baseId, refrId);
      continue;
    }
    actor->AddObjectToContainer(bound, nullptr, missing, nullptr);
    added += static_cast<size_t>(missing);
  }
  logger::info("[verb:npc-fate] inventory of {:x} reconciled to the record: "
               "{} row(s) and {} worn, {} removed ({} worn stack(s) "
               "unequipped first), {} added, {} kept",
               refrId, fate.inv.size(), fate.worn.size(), removed, unequipped,
               added, kept);
}

// The copy's worn set against the fate's worn list, as sets of base ids: the
// entries GetInventory lists as worn (InventoryEntryData::IsWorn, include/
// RE/I/InventoryEntryData.h:50; the leveled-list bases left out as in the
// reconcile). Equal, the detours or an earlier redress dressed this copy
// already and the strip, the re-add and the dress have nothing to do.
bool WornSetEquals(RE::Actor* actor, const Fates::Fate& fate)
{
  std::set<uint32_t> want(fate.worn.begin(), fate.worn.end());
  std::set<uint32_t> have;
  auto inventory = actor->GetInventory(
    [](RE::TESBoundObject& o) { return !o.Is(RE::FormType::LeveledItem); });
  for (auto& [object, entry] : inventory) {
    if (object && entry.first > 0 && entry.second && entry.second->IsWorn()) {
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
  if (WornSetEquals(actor, fate)) {
    ReconcileInventoryOnGameThread(actor, fate, refrId);
    logger::info("[verb:npc-fate] redressActor({:x}): the copy wears the "
                 "fate's {} already (3D loaded {}), no redress; its "
                 "inventory reconciled to the record",
                 refrId, fate.worn.size(), actor->Is3DLoaded());
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
               "of {} tagged for outfit {:x} and worn ({} already tagged, "
               "3D loaded {}), the record's rows reconciled first, the "
               "dress unqueued",
               refrId, dressed.added, fate.worn.size(), outfit->formID,
               dressed.carried, actor->Is3DLoaded());
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
  reroll(actor, -1);
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
  actor->ResetInventory(false);
  RedressOnGameThread(refrId, fate);
  if (had3D) {
    actor->DoReset3D(false);
  }
  logger::info("[verb:npc-fate] rerolled {:x} through the engine from pick "
               "{:x} toward the fate's {:x} (3D loaded {} before; the "
               "inventory reset, the record dressed, the 3D reset for the "
               "process's rebuild)",
               refrId, pickedId, fate.baseId, had3D);
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
  g_nativeCallRequirements.gameThrQ->AddTask([refrId, fate](Viet::Void) {
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
    if (!actor) {
      logger::warn("[verb:npc-fate] rerollActor({:x}): no such actor", refrId);
      return;
    }
    RerollOnGameThread(actor, fate, refrId);
  });
  return Napi::Boolean::New(info.Env(), true);
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
  // have to run anyway; the fate travels by value
  g_nativeCallRequirements.gameThrQ->AddTask(
    [refrId, fate](Viet::Void) { RedressOnGameThread(refrId, fate); });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value FateApi::ResurrectActor(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  if (refrId == 0 || refrId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  g_nativeCallRequirements.gameThrQ->AddTask([refrId](Viet::Void) {
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
    if (!actor) {
      logger::warn("[verb:npc-fate] resurrectActor({:x}): no such actor",
                   refrId);
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
    if (puppet) {
      actor->EnableAI(false);
      actor->SetActivationBlocked(true);
    } else {
      if (aiOff && actor->IsDead()) {
        // the resurrection did not take: the body keeps H-54e's AI off, so
        // no package walks it (Hooks.cpp HostedResurrect's shape); a stood-up
        // actor keeps the AI on, and a watcher's next pass demotes it
        actor->EnableAI(false);
      }
      actor->SetActivationBlocked(false);
    }
    // thuum docs/verbs/npc-fate.md 1e (x-fate-dress-bleak v5 20261104-070209,
    // c1-relaunch-080: "redressed 39fe4" at :534 and this stand-up in the
    // same millisecond, Resurrect(true, true)'s inventory reset re-dressing
    // the engine's own outfit over the redress): the record's dress follows
    // the reset HERE, in the same task, so the stood-up copy wears the fate
    // (the pick's swap, if any, is the late fate's rerollActor after this)
    Fates::Fate fate;
    bool fated = false;
    if (!actor->IsDead() && Fates::Enabled() && Fates::Get(refrId, fate)) {
      // the whole decision, not the redress alone: a respawn's fate that
      // names another pick re-rolls the stood-up clone through the engine
      // here, in the same task, after the engine's reset
      RerollOnGameThread(actor, fate, refrId);
      fated = true;
    }
    logger::info("[verb:npc-fate] resurrected {:x} (puppet {}, AI was off {}, "
                 "dead before {}, dead after {}, dressed from the fate {})",
                 refrId, puppet, aiOff, wasDead, actor->IsDead(), fated);
  });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value FateApi::ActorFateCount(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(info.Env(), static_cast<double>(Fates::Count()));
}
