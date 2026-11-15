#include "CellFollowsBinding.h"

Napi::Value CellFollowsBinding::Get(Napi::Env env, ScampServer& scampServer,
                                    uint32_t)
{
  const auto rows = scampServer.GetPartOne()->GetFollows();
  auto arr = Napi::Array::New(env, rows.size());
  for (size_t k = 0; k < rows.size(); ++k) {
    auto row = Napi::Object::New(env);
    row.Set("actor", Napi::Number::New(env, rows[k].actor));
    row.Set("player", Napi::Number::New(env, rows[k].player));
    row.Set("cell", Napi::Number::New(env, rows[k].cell));
    // thuum ADR-073 decision 6: a claim's purposes ("talk", "lead",
    // "scene"; none for a follow or a margin hold), its leads' quests, its
    // scene and its owner epoch
    auto purposes = Napi::Array::New(env);
    uint32_t n = 0;
    if (rows[k].talk) {
      purposes.Set(n++, Napi::String::New(env, "talk"));
    }
    if (!rows[k].leads.empty()) {
      purposes.Set(n++, Napi::String::New(env, "lead"));
    }
    if (rows[k].scene != 0) {
      purposes.Set(n++, Napi::String::New(env, "scene"));
    }
    row.Set("purposes", purposes);
    auto leads = Napi::Array::New(env, rows[k].leads.size());
    for (size_t i = 0; i < rows[k].leads.size(); ++i) {
      leads.Set(i, Napi::Number::New(env, rows[k].leads[i]));
    }
    row.Set("leads", leads);
    row.Set("scene", Napi::Number::New(env, rows[k].scene));
    row.Set("epoch", Napi::Number::New(env, rows[k].epoch));
    arr.Set(k, row);
  }
  return arr;
}
