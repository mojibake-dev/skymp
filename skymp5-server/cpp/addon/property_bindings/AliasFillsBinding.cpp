#include "AliasFillsBinding.h"

namespace {
// a form id of the running order, or null for none or for a plugin the order
// lacks; the cleared fill's empty desc reads as 0
Napi::Value FormValue(Napi::Env env, const std::optional<FormDesc>& desc,
                      const EspmFileList& files)
{
  if (!desc) {
    return env.Null();
  }
  if (desc->shortFormId == 0 && desc->file.empty()) {
    return Napi::Number::New(env, 0);
  }
  try {
    return Napi::Number::New(env, desc->ToFormId(files));
  } catch (const std::exception&) {
    return env.Null();
  }
}
}

Napi::Value AliasFillsBinding::Get(Napi::Env env, ScampServer& scampServer,
                                   uint32_t)
{
  auto& partOne = scampServer.GetPartOne();
  const auto& files = partOne->worldState.espmFiles;
  const auto fills = partOne->GetAliasFills();
  auto arr = Napi::Array::New(env);
  uint32_t k = 0;
  for (const auto& fill : fills) {
    auto row = Napi::Object::New(env);
    row.Set("quest", FormValue(env, fill.quest, files));
    row.Set("alias", Napi::Number::New(env, fill.alias));
    row.Set("refr", FormValue(env, fill.refr, files));
    row.Set("created", FormValue(env, fill.created, files));
    row.Set("container", FormValue(env, fill.container, files));
    row.Set("added", Napi::Boolean::New(env, fill.added));
    arr.Set(k++, row);
  }
  return arr;
}
