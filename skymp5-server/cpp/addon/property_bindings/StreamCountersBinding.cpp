#include "StreamCountersBinding.h"
#include "MpActor.h"

Napi::Value StreamCountersBinding::Get(Napi::Env env, ScampServer& scampServer,
                                       uint32_t)
{
  auto& partOne = *scampServer.GetPartOne();
  const auto rows = partOne.serverState.StreamRows();
  auto arr = Napi::Array::New(env, rows.size());
  for (size_t k = 0; k < rows.size(); ++k) {
    auto row = Napi::Object::New(env);
    MpActor* actor = partOne.serverState.ActorByUser(rows[k].user);
    row.Set("user", Napi::Number::New(env, rows[k].user));
    row.Set("player", Napi::Number::New(env, actor ? actor->GetFormId() : 0u));
    row.Set("t", Napi::Number::New(env, rows[k].msgType));
    row.Set("accepted", Napi::Number::New(env, rows[k].accepted));
    row.Set("dropped", Napi::Number::New(env, rows[k].dropped));
    row.Set("foreign", Napi::Number::New(env, rows[k].foreign));
    arr.Set(k, row);
  }
  return arr;
}
