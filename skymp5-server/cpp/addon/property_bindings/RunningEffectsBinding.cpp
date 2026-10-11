#include "RunningEffectsBinding.h"

Napi::Value RunningEffectsBinding::Get(Napi::Env env,
                                       ScampServer& scampServer,
                                       uint32_t formId)
{
  auto& partOne = scampServer.GetPartOne();
  auto& actor = partOne->worldState.GetFormAt<MpActor>(formId);
  const auto effects = actor.GetRunningEffects();
  auto result = Napi::Array::New(env, effects.size());
  for (size_t i = 0; i < effects.size(); ++i) {
    auto entry = Napi::Object::New(env);
    entry.Set("effect", Napi::Number::New(env, effects[i].effect));
    entry.Set("source", Napi::Number::New(env, effects[i].source));
    entry.Set("magnitude", Napi::Number::New(env, effects[i].magnitude));
    entry.Set("secondsLeft", Napi::Number::New(env, effects[i].secondsLeft));
    result.Set(static_cast<uint32_t>(i), entry);
  }
  return result;
}
