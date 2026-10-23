#include "FateApi.h"
#include "CallNativeApi.h"
#include "Fates.h"
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
  const auto baseId = fate.baseId;
  g_nativeCallRequirements.gameThrQ->AddTask([refrId, baseId](Viet::Void) {
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(refrId);
    if (!actor) {
      logger::warn("[verb:npc-fate] rerollActor({:x}): no such actor", refrId);
      return;
    }
    REL::Relocation<ActorRerollFn> reroll{ Offsets::Fate::ActorReroll };
    reroll(actor, -1);
    logger::info("[verb:npc-fate] rerolled {:x} through the engine (3D loaded "
                 "{}) toward the fate's {:x}",
                 refrId, actor->Is3DLoaded(), baseId);
  });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value FateApi::ActorFateCount(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(info.Env(), static_cast<double>(Fates::Count()));
}
