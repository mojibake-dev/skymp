#include "WorldGlobalsBinding.h"

Napi::Value WorldGlobalsBinding::Get(Napi::Env env, ScampServer& scampServer,
                                     uint32_t)
{
  auto& partOne = scampServer.GetPartOne();
  const auto [streamer, epoch] = partOne->WorldStreamer();
  auto res = Napi::Object::New(env);
  res.Set("streamer", Napi::Number::New(env, streamer));
  res.Set("epoch", Napi::Number::New(env, epoch));
  auto entries = Napi::Array::New(env);
  uint32_t k = 0;
  for (const auto& global : partOne->GetWorldGlobals()) {
    uint32_t formId = 0;
    try {
      formId = global.global.ToFormId(partOne->worldState.espmFiles);
    } catch (const std::exception&) {
      continue;
    }
    auto entry = Napi::Object::New(env);
    entry.Set("global", Napi::Number::New(env, formId));
    entry.Set("value", Napi::Number::New(env, global.value));
    entries.Set(k++, entry);
  }
  res.Set("entries", entries);
  return res;
}
