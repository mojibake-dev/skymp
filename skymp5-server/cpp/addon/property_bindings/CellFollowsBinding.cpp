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
    arr.Set(k, row);
  }
  return arr;
}
