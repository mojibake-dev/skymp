#pragma once
#include "PropertyBinding.h"

// mp.get(refrId, "isDeleted"): whether the server's record holds the
// reference deleted (thuum ADR-068 rung 4: an owner's recorded Delete keeps a
// game-file reference deleted and disabled on every seat); read-only, for the
// lab's claims (labState's ref isDeleted)
class IsDeletedBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "isDeleted"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
