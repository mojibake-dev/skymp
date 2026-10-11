#pragma once
#include "PropertyBinding.h"

// mp.get(actor, "shouts"): what the server holds of an actor's shouts (thuum
// docs/verbs/shouts.md), {shouts, wordsKnown, wordsUnlocked} as form ids and
// recoverySecondsLeft, its voice's recovery on the server's clock;
// read-only: the record changes by the console and the cast rule only.
class ShoutsBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "shouts"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
