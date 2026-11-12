#include "ScriptVariablesBinding.h"

Napi::Value ScriptVariablesBinding::Get(Napi::Env env,
                                        ScampServer& scampServer, uint32_t)
{
  auto& partOne = scampServer.GetPartOne();
  auto arr = Napi::Array::New(env);
  uint32_t k = 0;
  for (const auto& variable : partOne->GetScriptVariables()) {
    uint32_t form = 0;
    try {
      form = variable.form.ToFormId(partOne->worldState.espmFiles);
    } catch (const std::exception&) {
      continue;
    }
    auto row = Napi::Object::New(env);
    row.Set("form", Napi::Number::New(env, form));
    row.Set("script", Napi::String::New(env, variable.script));
    row.Set("var", Napi::String::New(env, variable.var));
    row.Set("k", Napi::Number::New(env, variable.k));
    if (variable.i) {
      row.Set("i", Napi::Number::New(env, *variable.i));
    }
    if (variable.f) {
      row.Set("f", Napi::Number::New(env, *variable.f));
    }
    if (variable.b) {
      row.Set("b", Napi::Boolean::New(env, *variable.b));
    }
    row.Set("seq", Napi::Number::New(env, variable.seq));
    arr.Set(k++, row);
  }
  return arr;
}
