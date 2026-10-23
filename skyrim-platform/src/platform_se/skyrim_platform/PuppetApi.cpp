#include "PuppetApi.h"
#include "CallNativeApi.h"
#include "Puppets.h"
#include "game/Offsets.h"
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
struct
{
  std::unordered_set<uint32_t> ids;
  std::unordered_map<uint32_t, Puppets::Locomotion> locomotion;
  std::recursive_mutex m;
} g_puppets;

// Actor::ClearKeepOffsetFromActor is not in CommonLibSSE-NG (only the
// IMovementSetKeepOffsetFromActor vtable is). The engine's function (37895;
// REA on sky-re 2026-10-21, CONFIRMED-STATIC: `void (RE::Actor*)`, it flips
// the movement controller's keep-offset mode byte and nothing else)
using ClearKeepOffsetFn = void (*)(RE::Actor*);

// re-ai-puppet.md Q4, live to puppet: combat and alarm off, the keep-offset
// left, the AI bit cleared (EnableAI(false) also zeroes the character
// controller's velocity, src/RE/A/Actor.cpp:230-240); the actor stays in
// high process, where the detours animate it
void Demote(RE::Actor& actor)
{
  // an actor with no 3D (a persistent one its game has left behind, demoted
  // when the server took its form back, skymp5-client formView.destroy)
  // has no combat, no movement controller and no process to stop: the bit
  // alone, which EnableAI(false) writes without touching a controller
  // (src/RE/A/Actor.cpp:230-240, GetCharController null-safe)
  if (!actor.Is3DLoaded()) {
    actor.EnableAI(false);
    logger::info("[verb:npc-puppet] puppet {:x}: AI off (no 3D, the bit "
                 "alone)",
                 actor.formID);
    return;
  }
  if (auto* lists = RE::ProcessLists::GetSingleton()) {
    lists->StopCombatAndAlarmOnActor(&actor, false);
  }
  // the engine's clear reads the movement controller with no null test
  // (37895 to 41734(controller, 0), REA 2026-10-21); an actor whose process
  // has not started has none yet (GetActorRuntimeData().movementController,
  // Actor.h's runtime data), so the clear waits for a later call
  if (actor.GetActorRuntimeData().movementController.get() != nullptr) {
    REL::Relocation<ClearKeepOffsetFn> clearKeepOffset{
      Offsets::Puppet::ClearKeepOffsetFromActor
    };
    clearKeepOffset(&actor);
  }
  actor.EnableAI(false);
  logger::info("[verb:npc-puppet] puppet {:x}: AI off, combat stopped, "
               "keep-offset cleared",
               actor.formID);
}

// puppet to live: the AI bit set and the package evaluated on the first AI
// tick (EvaluatePackage(false, false), Papyrus's shape; an immediate one runs
// only in sit states 0, 3 and 7, re-ai-puppet.md:341-348). The relayed point
// and heading are written by skymp5-client before this call.
void Promote(RE::Actor& actor)
{
  actor.EnableAI(true);
  actor.EvaluatePackage(false, false);
  logger::info("[verb:npc-puppet] live {:x}: AI on, package evaluated",
               actor.formID);
}
}

bool Puppets::Is(uint32_t formId)
{
  std::lock_guard l(g_puppets.m);
  return g_puppets.ids.count(formId) != 0;
}

void Puppets::Set(uint32_t formId, bool puppet)
{
  std::lock_guard l(g_puppets.m);
  if (puppet) {
    g_puppets.ids.insert(formId);
  } else {
    g_puppets.ids.erase(formId);
    g_puppets.locomotion.erase(formId);
  }
}

size_t Puppets::Count()
{
  std::lock_guard l(g_puppets.m);
  return g_puppets.ids.size();
}

uint64_t Puppets::NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

void Puppets::SetLocomotion(uint32_t formId, const Locomotion& sample)
{
  std::lock_guard l(g_puppets.m);
  Locomotion stamped = sample;
  stamped.stampMs = NowMs();
  g_puppets.locomotion[formId] = stamped;
}

bool Puppets::GetLocomotion(uint32_t formId, Locomotion& out)
{
  std::lock_guard l(g_puppets.m);
  const auto it = g_puppets.locomotion.find(formId);
  if (it == g_puppets.locomotion.end()) {
    return false;
  }
  out = it->second;
  return true;
}

Napi::Value PuppetApi::SetPuppet(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto puppet = NapiHelper::ExtractBoolean(info[1], "puppet");
  if (actorFormId == 0 || actorFormId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  // the flag first: the detours read it from this frame on. The actor is
  // looked up on the game thread alone (performAction's shape): a lookup
  // here runs on the JS thread under the forms map's lock while the game
  // thread waits for this tick, and a cell load holding that lock would
  // hold both (the gate's hang at a landing, 2026-10-21, under reading)
  Puppets::Set(actorFormId, puppet);
  g_nativeCallRequirements.gameThrQ->AddTask(
    [actorFormId, puppet](Viet::Void) {
      auto* pActor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
      if (!pActor) {
        logger::warn("[verb:npc-puppet] setPuppet({:x}, {}): no such actor",
                     actorFormId, puppet);
        Puppets::Set(actorFormId, false);
        return;
      }
      if (puppet) {
        Demote(*pActor);
      } else {
        Promote(*pActor);
      }
    });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value PuppetApi::IsPuppet(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  return Napi::Boolean::New(info.Env(), Puppets::Is(actorFormId));
}

// setPuppetLocomotion(actorFormId, speed, directionDeg, turnRateDeg): the
// relayed sample a puppet's graph is fed from, stored under the registry's
// lock on the JS thread (no game call); false when the actor is not a
// puppet (the sample is kept anyway for the next demote)
Napi::Value PuppetApi::SetPuppetLocomotion(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  Puppets::Locomotion sample;
  sample.speed = NapiHelper::ExtractFloat(info[1], "speed");
  sample.directionDeg = NapiHelper::ExtractFloat(info[2], "directionDeg");
  sample.turnRateDeg = NapiHelper::ExtractFloat(info[3], "turnRateDeg");
  Puppets::SetLocomotion(actorFormId, sample);
  return Napi::Boolean::New(info.Env(), Puppets::Is(actorFormId));
}
