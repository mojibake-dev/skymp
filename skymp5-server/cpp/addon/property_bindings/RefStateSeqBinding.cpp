#include "RefStateSeqBinding.h"
#include "MpObjectReference.h"
#include "WorldState.h"

Napi::Value RefStateSeqBinding::Get(Napi::Env env, ScampServer& scampServer,
                                    uint32_t formId)
{
  auto& partOne = scampServer.GetPartOne();
  const auto& form = partOne->worldState.LookupFormByIdNoLoad(formId);
  MpObjectReference* refr = form ? form->AsObjectReference() : nullptr;
  const auto seq = refr ? refr->GetRefStateSeq() : std::nullopt;
  if (!seq) {
    return env.Null();
  }
  return Napi::Number::New(env, *seq);
}
