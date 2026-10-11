#pragma once
#include "PropertyBinding.h"

// mp.get(actor, "runningEffects"): the effects running on an actor (thuum
// docs/verbs/magic-effects.md), each {effect, source, magnitude,
// secondsLeft}, read-only: the server applies effects only from the records.
class RunningEffectsBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "runningEffects"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
