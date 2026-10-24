#pragma once

#include "NapiHelper.h"

namespace PuppetApi {

// thuum docs/verbs/npc-puppet.md: setPuppet(actorFormId, puppet). true: the
// demote, re-ai-puppet.md Q4 (combat and alarm stopped, the keep-offset
// cleared, the AI bit cleared, the actor flagged for the detours); false:
// the promote (unflagged, the AI bit set, the package evaluated). The flag
// flips at once; the engine calls run on the game thread right after this
// update (performAction's shape). Returns false when no such actor exists.
Napi::Value SetPuppet(const Napi::CallbackInfo& info);
// isPuppet(actorFormId): whether this game flagged the actor
Napi::Value IsPuppet(const Napi::CallbackInfo& info);
// releasePuppet(actorFormId): a puppet this engine no longer loads is left
// to its own schedule (thuum docs/verbs/npc-handoff.md D11): the flag off at
// once, then on the game thread the activation unblocked and the AI bit set,
// no package evaluation (an unloaded actor has no process). False for an
// invalid id. The demote poll makes it a puppet again within pollMs when it
// loads on a seat that does not host it; the host's roster sights it.
Napi::Value ReleasePuppet(const Napi::CallbackInfo& info);
// setPuppetLocomotion(actorFormId, speed, directionDeg, turnRateDeg): the
// host's latest movement sample, which the Move detour writes every frame
// where the graph's Speed, Direction and TurnDelta channels read
// (docs/research/m3/re-puppet-locomotion.md)
Napi::Value SetPuppetLocomotion(const Napi::CallbackInfo& info);
// getHighProcessActors(): the engine's high-process actors by form id, the
// game thread's latest snapshot (Puppets.h)
Napi::Value GetHighProcessActors(const Napi::CallbackInfo& info);

inline void Register(Napi::Env env, Napi::Object& exports)
{
  exports.Set("getHighProcessActors",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(GetHighProcessActors)));
  exports.Set(
    "setPuppet",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetPuppet)));
  exports.Set(
    "isPuppet",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(IsPuppet)));
  exports.Set(
    "releasePuppet",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ReleasePuppet)));
  exports.Set("setPuppetLocomotion",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetPuppetLocomotion)));
}
}
