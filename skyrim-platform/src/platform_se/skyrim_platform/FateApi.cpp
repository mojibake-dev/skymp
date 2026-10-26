#include "FateApi.h"
#include "CallNativeApi.h"
#include "Fates.h"
#include "Puppets.h"
#include "game/Offsets.h"
#include <string>

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
void RedressOnGameThread(uint32_t refrId, const Fates::Fate& fate)
{
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
  if (!actor) {
    logger::warn("[verb:npc-fate] redressActor({:x}): no such actor", refrId);
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
  // re-npc-roll.md 8.4's three calls, CommonLib's SetDefaultOutfit
  // sequence (src/RE/A/Actor.cpp:1016-1029) less the outfit change:
  // Actor::RemoveOutfitItems (include/RE/A/Actor.h:617, bound to 19690 at
  // src/RE/A/Actor.cpp:1300-1305) with the outfit, not nullptr, to spare
  // the sleep outfit's entries; the same adds as the detour's; Actor::
  // AddWornOutfit (include/RE/A/Actor.h:502, bound to 19692 at :1293-1298)
  // with a_forceUpdate true: one pop
  actor->RemoveOutfitItems(outfit);
  const auto dressed = Fates::AddWornTagged(changes, outfit, fate, refrId);
  actor->AddWornOutfit(outfit, true);
  logger::info("[verb:npc-fate] redressed {:x} from the server's roll: {} "
               "of {} tagged for outfit {:x} and worn ({} already tagged, "
               "3D loaded {})",
               refrId, dressed.added, fate.worn.size(), outfit->formID,
               dressed.carried, actor->Is3DLoaded());
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
    REL::Relocation<ActorRerollFn> reroll{ Offsets::Fate::ActorReroll };
    reroll(actor, -1);
    logger::info("[verb:npc-fate] rerolled {:x} through the engine (3D loaded "
                 "{}) toward the fate's {:x}",
                 refrId, actor->Is3DLoaded(), fate.baseId);
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
    // Actor::EnableAI (A/Actor.h:521), Actor::Resurrect (A/Actor.h:369),
    // TESObjectREFR::SetActivationBlocked (T/TESObjectREFR.h:464)
    if (puppet) {
      actor->EnableAI(true);
    }
    actor->Resurrect(true, true);
    if (puppet) {
      actor->EnableAI(false);
      actor->SetActivationBlocked(true);
    } else {
      actor->SetActivationBlocked(false);
    }
    logger::info(
      "[verb:npc-fate] resurrected {:x} (puppet {}, dead before {}, "
      "dead after {})",
      refrId, puppet, wasDead, actor->IsDead());
  });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value FateApi::ActorFateCount(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(info.Env(), static_cast<double>(Fates::Count()));
}
