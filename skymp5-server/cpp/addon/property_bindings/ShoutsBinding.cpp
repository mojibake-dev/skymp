#include "ShoutsBinding.h"

Napi::Value ShoutsBinding::Get(Napi::Env env, ScampServer& scampServer,
                               uint32_t formId)
{
  auto& partOne = scampServer.GetPartOne();
  auto& actor = partOne->worldState.GetFormAt<MpActor>(formId);
  const auto record = actor.GetShoutRecord();
  const auto& files = partOne->worldState.espmFiles;
  const auto ids = [&](const std::vector<FormDesc>& forms) {
    auto result = Napi::Array::New(env, forms.size());
    for (size_t i = 0; i < forms.size(); ++i) {
      result.Set(static_cast<uint32_t>(i),
                 Napi::Number::New(env, forms[i].ToFormId(files)));
    }
    return result;
  };
  auto result = Napi::Object::New(env);
  result.Set("shouts", ids(record.shouts));
  result.Set("wordsKnown", ids(record.wordsKnown));
  result.Set("wordsUnlocked", ids(record.wordsUnlocked));
  result.Set(
    "recoverySecondsLeft",
    Napi::Number::New(env, partOne->VoiceRecoveryLeftMs(formId) / 1000.0));
  return result;
}
