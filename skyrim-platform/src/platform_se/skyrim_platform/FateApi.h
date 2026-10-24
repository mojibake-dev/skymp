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
// rerollActor(refrId): the late path for a fate that arrived after the roll
// (one visible pop): on the game thread, a leveled clone (ExtraLeveledCreature
// on the reference) takes the engine's own re-roll (37323(actor, -1),
// re-npc-roll.md section 3 (b)) through the detours, any other actor the
// redress below; the decision is the engine's mark, never a Papyrus base
// read (GetBaseObject answers a clone's placed base). False when the
// reference has no fate or the switch is off, since the engine would then
// roll its own.
Napi::Value RerollActor(const Napi::CallbackInfo& info);
// redressActor(refrId): for a fate that arrives after an unleveled actor was
// dressed (re-npc-roll.md section 8.4's late path): on the game thread, for
// a fated actor with a base defaultOutfit, the engine's RemoveOutfitItems
// with that outfit, the server's worn list added tagged exactly as the
// outfit detour adds it, then AddWornOutfit with a_forceUpdate: one pop.
// False when the id is invalid, the reference holds no fate or the switch is
// off; a reference the game cannot find, or a base with no default outfit,
// is a warn on the game thread (the lookup runs there, setPuppet's shape).
Napi::Value RedressActor(const Napi::CallbackInfo& info);
// resurrectActor(refrId): a game-file actor's respawn, the engine's own
// Resurrect (A/Actor.h:369: the inventory reset, the 3D attached) run with
// its AI on, in one game-thread task: a puppet is given its AI back for the
// call and demoted again after it (EnableAI false, activation blocked), a
// live actor is unblocked (the body's block from the kill). x-npc-respawn
// 070508: Resurrect from Papyrus on a corpse a reconnecting viewer had just
// demoted (AI off, after an AI-on death) did not stand it up, while the
// same corpse stood up on the seat whose promote (AI on) ran 40 ms before.
// False for an invalid id; a reference the game cannot find is a warn on
// the game thread (the lookup runs there, setPuppet's shape).
Napi::Value ResurrectActor(const Napi::CallbackInfo& info);
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
    "redressActor",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(RedressActor)));
  exports.Set(
    "resurrectActor",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ResurrectActor)));
  exports.Set(
    "actorFateCount",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ActorFateCount)));
}
}
