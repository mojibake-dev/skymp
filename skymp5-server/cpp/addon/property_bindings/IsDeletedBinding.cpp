#include "IsDeletedBinding.h"
#include "MpObjectReference.h"
#include "WorldState.h"

Napi::Value IsDeletedBinding::Get(Napi::Env env, ScampServer& scampServer,
                                  uint32_t formId)
{
  auto& partOne = scampServer.GetPartOne();
  const auto& form = partOne->worldState.LookupFormByIdNoLoad(formId);
  MpObjectReference* refr = form ? form->AsObjectReference() : nullptr;
  if (!refr) {
    return env.Null();
  }
  return Napi::Boolean::New(env, refr->IsDeleted());
}
