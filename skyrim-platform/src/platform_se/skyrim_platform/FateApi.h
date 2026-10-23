#pragma once

#include "NapiHelper.h"

namespace FateApi {

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the server's
// one roll for a leveled placement, held by this game ahead of the engine's
// own roll (Fates.h). setActorFate(refrId, baseId, worn): the NPC_ the server
// named (0: none, the engine deletes the placement) and the worn items it
// rolled; stored on the JS thread, no game call; the detours (FateHooks.cpp)
// read it at the roll. Returns false for an invalid reference id.
Napi::Value SetActorFate(const Napi::CallbackInfo& info);
// clearActorFate(refrId): the reference rolls as the engine's own again;
// false when it held no fate
Napi::Value ClearActorFate(const Napi::CallbackInfo& info);
// setActorFateEnabled(on): the runtime switch over both detours, default off
Napi::Value SetActorFateEnabled(const Napi::CallbackInfo& info);
// rerollActor(refrId): the engine's own re-roll of a loaded leveled actor
// (37323(actor, -1), re-npc-roll.md section 3 (b)), through the detours, for
// a fate that arrived after the roll: one visible pop. The actor is looked up
// on the game thread (setPuppet's shape); false when the reference has no
// fate or the switch is off, since the engine would then roll its own.
Napi::Value RerollActor(const Napi::CallbackInfo& info);
// actorFateCount(): the table's size
Napi::Value ActorFateCount(const Napi::CallbackInfo& info);

inline void Register(Napi::Env env, Napi::Object& exports)
{
  exports.Set(
    "setActorFate",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetActorFate)));
  exports.Set(
    "clearActorFate",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ClearActorFate)));
  exports.Set("setActorFateEnabled",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetActorFateEnabled)));
  exports.Set(
    "rerollActor",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(RerollActor)));
  exports.Set(
    "actorFateCount",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ActorFateCount)));
}
}
