#include "PuppetApi.h"
#include "CallNativeApi.h"
#include "Puppets.h"
#include "game/Offsets.h"
#include <mutex>
#include <unordered_set>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
struct
{
  std::unordered_set<uint32_t> ids;
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
  if (auto* lists = RE::ProcessLists::GetSingleton()) {
    lists->StopCombatAndAlarmOnActor(&actor, false);
  }
  REL::Relocation<ClearKeepOffsetFn> clearKeepOffset{
    Offsets::Puppet::ClearKeepOffsetFromActor
  };
  clearKeepOffset(&actor);
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
  }
}

size_t Puppets::Count()
{
  std::lock_guard l(g_puppets.m);
  return g_puppets.ids.size();
}

Napi::Value PuppetApi::SetPuppet(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto puppet = NapiHelper::ExtractBoolean(info[1], "puppet");
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
  if (!actor) {
    return Napi::Boolean::New(info.Env(), false);
  }
  // the flag first: the detours read it from this frame on
  Puppets::Set(actorFormId, puppet);
  g_nativeCallRequirements.gameThrQ->AddTask(
    [actorFormId, puppet](Viet::Void) {
      auto* pActor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
      if (!pActor) {
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
