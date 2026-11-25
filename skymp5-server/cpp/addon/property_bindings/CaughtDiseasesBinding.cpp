#include "CaughtDiseasesBinding.h"

Napi::Value CaughtDiseasesBinding::Get(Napi::Env env, ScampServer& scampServer,
                                       uint32_t formId)
{
  auto& partOne = scampServer.GetPartOne();
  auto& actor = partOne->worldState.GetFormAt<MpActor>(formId);
  const auto changeForm = actor.GetChangeForm();
  const auto& caught = changeForm.caughtDiseases.Get();
  auto result = Napi::Array::New(env, caught.size());
  uint32_t i = 0;
  for (const auto& [spellId, day] : caught) {
    auto entry = Napi::Object::New(env);
    entry.Set("spell", Napi::Number::New(env, spellId));
    entry.Set("caughtAt", Napi::Number::New(env, day));
    result.Set(i++, entry);
  }
  return result;
}
