#include "QuestRunsBinding.h"

Napi::Value QuestRunsBinding::Get(Napi::Env env, ScampServer& scampServer,
                                  uint32_t)
{
  auto& partOne = scampServer.GetPartOne();
  auto arr = Napi::Array::New(env);
  uint32_t k = 0;
  for (const auto& run : partOne->GetQuestRuns()) {
    uint32_t quest = 0;
    try {
      quest = run.quest.ToFormId(partOne->worldState.espmFiles);
    } catch (const std::exception&) {
      continue;
    }
    auto row = Napi::Object::New(env);
    row.Set("quest", Napi::Number::New(env, quest));
    row.Set("running", Napi::Boolean::New(env, run.running));
    row.Set("seq", Napi::Number::New(env, run.seq));
    arr.Set(k++, row);
  }
  return arr;
}
