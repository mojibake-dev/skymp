#include "BodyRestBinding.h"
#include "MpActor.h"
#include "WorldState.h"

Napi::Value BodyRestBinding::Get(Napi::Env env, ScampServer& scampServer,
                                 uint32_t formId)
{
  auto& worldState = scampServer.GetPartOne()->worldState;
  const auto& form = worldState.LookupFormByIdNoLoad(formId);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor) {
    return env.Null();
  }
  auto res = Napi::Object::New(env);
  res.Set("atRest", Napi::Boolean::New(env, actor->IsBodyAtRest()));
  res.Set("restAt",
          Napi::Number::New(env, static_cast<double>(actor->BodyRestAtMs())));
  res.Set("owner", Napi::Number::New(env, actor->BodyOwnerId()));
  res.Set("samples", Napi::Number::New(env, actor->BodySampleCount()));
  return res;
}
