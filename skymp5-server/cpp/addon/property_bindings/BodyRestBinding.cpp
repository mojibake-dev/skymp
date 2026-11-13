#include "BodyRestBinding.h"
#include "MpActor.h"
#include "WorldState.h"

Napi::Value BodyRestBinding::Get(Napi::Env env, ScampServer& scampServer,
                                 uint32_t formId)
{
  const auto& partOne = scampServer.GetPartOne();
  auto& worldState = partOne->worldState;
  const auto& form = worldState.LookupFormByIdNoLoad(formId);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor) {
    return env.Null();
  }
  auto res = Napi::Object::New(env);
  res.Set("atRest", Napi::Boolean::New(env, actor->IsBodyAtRest()));
  res.Set("restAt",
          Napi::Number::New(env, static_cast<double>(actor->BodyRestAtMs())));
  // physics rung 5a: the body's live row's owner, 0 when none
  res.Set("owner", Napi::Number::New(env, partOne->PropLiveOwner(*actor)));
  res.Set("samples", Napi::Number::New(env, actor->BodySampleCount()));
  return res;
}
