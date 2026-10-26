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
// thuum docs/verbs/npc-stream.md (ADR-050): setHosted(actorFormId, hosted),
// the server's word that this game simulates the actor (true) or not
// (false), kept in the hosted set the hook on the engine's action entry
// reads (Hosted.h); the JS thread alone, no game call. False for 0 and for
// the player's own id (its actions are reported regardless).
Napi::Value SetHosted(const Napi::CallbackInfo& info);
// isHosted(actorFormId): whether the set holds the actor
Napi::Value IsHosted(const Napi::CallbackInfo& info);
// isPerformingAction(actorFormId): whether the engine's action entry is open
// on the actor right now (Hosted::IsPerforming, the hook's mark); read
// inside a sendAnimationEvent hook handler, where the game thread waits, it
// says the event being sent is the action's own
Napi::Value IsPerformingAction(const Napi::CallbackInfo& info);
// thuum docs/verbs/npc-stream.md (the voice slice): sayTopicInfo(actorFormId,
// topicInfoFormId) makes a puppet say the host's line through the engine's
// own say (Offsets::Voice::SayTopic with the info given, so the engine's
// pick is skipped), queued to the game thread; sayTopicInfoResult answers
// -1 while it waits, 0 for no actor, info or process, 1 when the line
// started (the actor's sound callback state read 3 after the call), 2 when
// the engine refused it (a line still ending, the process's dialogue state),
// 3 when a line plays without the state byte (a live sound handle or a
// running timer: the sound descriptor path)
Napi::Value SayTopicInfo(const Napi::CallbackInfo& info);
Napi::Value SayTopicInfoResult(const Napi::CallbackInfo& info);

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
  exports.Set(
    "setHosted",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetHosted)));
  exports.Set(
    "isHosted",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(IsHosted)));
  exports.Set("isPerformingAction",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(IsPerformingAction)));
  exports.Set(
    "sayTopicInfo",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SayTopicInfo)));
  exports.Set("sayTopicInfoResult",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SayTopicInfoResult)));
}
}
