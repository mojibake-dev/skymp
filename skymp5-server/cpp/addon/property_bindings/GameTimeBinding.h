#pragma once
#include "PropertyBinding.h"

// mp.get(0, "gameTime"): the server's game clock now (thuum
// docs/verbs/time.md), read-only.
class GameTimeBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "gameTime"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
