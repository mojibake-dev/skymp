#include "ChangeFormRecordBinding.h"
#include "MpActor.h"
#include "WorldState.h"

Napi::Value ChangeFormRecordBinding::Get(Napi::Env env,
                                         ScampServer& scampServer,
                                         uint32_t formId)
{
  auto& worldState = scampServer.GetPartOne()->worldState;
  const auto& form = worldState.LookupFormByIdNoLoad(formId);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor) {
    return env.Null();
  }
  const auto record = actor->GetChangeFormRecord();
  if (!record) {
    return env.Null();
  }
  auto res = Napi::Object::New(env);
  res.Set("fnv1a", Napi::Number::New(env, record->fnv1a));
  res.Set("at", Napi::Number::New(env, static_cast<double>(record->atMs)));
  res.Set("epoch", Napi::Number::New(env, record->epoch));
  res.Set("flags", Napi::Number::New(env, record->flags));
  res.Set("formType", Napi::Number::New(env, record->formType));
  res.Set("version", Napi::Number::New(env, record->version));
  res.Set("sender", Napi::Number::New(env, record->sender));
  res.Set("bodyLength",
          Napi::Number::New(env, static_cast<double>(record->BodyLength())));
  res.Set("remapRows",
          Napi::Number::New(env, static_cast<double>(record->remap.size())));
  return res;
}
