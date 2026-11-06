#include "ScriptStateBinding.h"
#include "MpObjectReference.h"
#include "WorldState.h"

Napi::Value ScriptStateBinding::Get(Napi::Env env, ScampServer& scampServer,
                                    uint32_t formId)
{
  auto& worldState = scampServer.GetPartOne()->worldState;
  const auto& form = worldState.LookupFormByIdNoLoad(formId);
  MpObjectReference* refr = form ? form->AsObjectReference() : nullptr;
  if (!refr) {
    return env.Null();
  }
  const auto& record = refr->GetScriptState();
  if (!record) {
    return env.Null();
  }
  auto res = Napi::Object::New(env);
  res.Set("fnv1a", Napi::Number::New(env, record->fnv1a));
  res.Set("at", Napi::Number::New(env, static_cast<double>(record->atMs)));
  res.Set("epoch", Napi::Number::New(env, record->epoch));
  res.Set("sender", Napi::Number::New(env, record->sender));
  res.Set("seq", Napi::Number::New(env, record->seq));
  if (record->event) {
    res.Set("event", Napi::String::New(env, *record->event));
  }
  res.Set("scripts",
          Napi::Number::New(env, static_cast<double>(record->scripts.size())));
  return res;
}
