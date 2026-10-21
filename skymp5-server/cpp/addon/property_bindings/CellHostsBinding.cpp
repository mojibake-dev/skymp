#include "CellHostsBinding.h"

Napi::Value CellHostsBinding::Get(Napi::Env env, ScampServer& scampServer,
                                  uint32_t)
{
  const auto rows = scampServer.GetPartOne()->GetCellHosts();
  auto arr = Napi::Array::New(env, rows.size());
  for (size_t k = 0; k < rows.size(); ++k) {
    auto row = Napi::Object::New(env);
    row.Set("cell", Napi::Number::New(env, rows[k].cell));
    row.Set("hosted", Napi::Boolean::New(env, rows[k].hosted));
    row.Set("player", Napi::Number::New(env, rows[k].player));
    row.Set("epoch", Napi::Number::New(env, rows[k].epoch));
    arr.Set(k, row);
  }
  return arr;
}
