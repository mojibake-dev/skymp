#pragma once

#include "Magic/AnimationGraphMasterBehaviourDescriptor.h"
#include "NapiHelper.h"

namespace MagicApi {

Napi::Value CastSpellImmediate(const Napi::CallbackInfo& info);
Napi::Value InterruptCast(const Napi::CallbackInfo& info);

Napi::Value GetAnimationVariablesFromActor(const Napi::CallbackInfo& info);
Napi::Value ApplyAnimationVariablesToActor(const Napi::CallbackInfo& info);

// thuum docs/verbs/cast-action.md: performs a BGSAction on an actor through
// the engine's action entry with the idle given, the way the engine performs
// the caster's own; the figure's cast animation
Napi::Value PerformAction(const Napi::CallbackInfo& info);
// the graph's answer to the actor's last performAction: 1 taken, 0 refused,
// -1 none or still queued (it is performed right after the update that asked)
Napi::Value PerformActionResult(const Napi::CallbackInfo& info);
// whether the actor's graph has answered a cast request for the hand (0 left,
// 1 right, 2 voice) since the last call: the engine's cue to perform a start
Napi::Value CastReady(const Napi::CallbackInfo& info);
// hides or shows a reference's activation prompt text (ExtraFlags
// kBlockActivateText): an invisible figure's crosshair prompt
Napi::Value SetActivateTextHidden(const Napi::CallbackInfo& info);
// a figure's aim pitch (radians, down positive): its reference angle.x and
// its AimPitchCurrent, which the engine's GetAimAngle reads for its stream
Napi::Value SetActorAimPitch(const Napi::CallbackInfo& info);

// an actor's master-behavior variables as the JS object
// getAnimationVariablesFromActor returns (booleans, floats, integers as
// ArrayBuffers)
Napi::Object AnimationVariablesToJs(
  Napi::Env env,
  const AnimationGraphMasterBehaviourDescriptor::AnimationVariables& vars);

void Register(Napi::Env env, Napi::Object& exports);

}
