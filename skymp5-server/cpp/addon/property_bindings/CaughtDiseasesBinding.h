#pragma once
#include "PropertyBinding.h"

// mp.get(actor, "caughtDiseases"): the diseases the actor's own engine caught
// as the record holds them, [{spell, caughtAt}], the spell a form id and
// caughtAt the shared clock's day its effect first ran there; read-only: the
// record changes by the actor's own effects report and its own removal only.
class CaughtDiseasesBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "caughtDiseases"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
