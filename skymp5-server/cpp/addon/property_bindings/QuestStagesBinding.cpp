#include "QuestStagesBinding.h"

Napi::Value QuestStagesBinding::Get(Napi::Env env, ScampServer& scampServer,
                                    uint32_t)
{
  auto& partOne = scampServer.GetPartOne();
  auto arr = Napi::Array::New(env);
  // a read makes no journal (PartOne::GetQuestStages would)
  if (!partOne->worldState.LookupFormById(PartOne::kWorldJournalRefId)) {
    return arr;
  }
  uint32_t k = 0;
  for (const auto& done : partOne->GetQuestStages()) {
    uint32_t quest = 0;
    try {
      quest = done.quest.ToFormId(partOne->worldState.espmFiles);
    } catch (const std::exception&) {
      continue;
    }
    auto row = Napi::Object::New(env);
    row.Set("quest", Napi::Number::New(env, quest));
    row.Set("stage", Napi::Number::New(env, done.stage));
    arr.Set(k++, row);
  }
  return arr;
}
