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

// an actor's master-behavior variables as the JS object
// getAnimationVariablesFromActor returns (booleans, floats, integers as
// ArrayBuffers)
Napi::Object AnimationVariablesToJs(
  Napi::Env env,
  const AnimationGraphMasterBehaviourDescriptor::AnimationVariables& vars);

void Register(Napi::Env env, Napi::Object& exports);

}
