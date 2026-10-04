#include "GameTimeBinding.h"

Napi::Value GameTimeBinding::Get(Napi::Env env, ScampServer& scampServer,
                                 uint32_t)
{
  auto t = scampServer.GetPartOne()->GetGameTime();
  auto result = Napi::Object::New(env);
  result.Set("year", Napi::Number::New(env, t.year));
  result.Set("month", Napi::Number::New(env, t.month));
  result.Set("day", Napi::Number::New(env, t.day));
  result.Set("hour", Napi::Number::New(env, t.hour));
  result.Set("daysPassed", Napi::Number::New(env, t.daysPassed));
  result.Set("timeScale", Napi::Number::New(env, t.timeScale));
  return result;
}
