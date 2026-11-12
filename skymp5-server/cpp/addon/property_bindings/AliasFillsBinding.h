#pragma once
#include "PropertyBinding.h"

// mp.get(0, "aliasFills"): the world's quest alias fills and created items'
// marks as the server keeps them on its journal (thuum ADR-068 rung 4,
// AliasFill 79 and the created half): [{quest, alias, refr, created,
// container, added}], the forms as form ids of the running load order; refr
// 0 for a cleared alias and null where no seat filled it; created and
// container null where the alias creates nothing; read-only, for the lab's
// claims (labState's alias_fill).
class AliasFillsBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "aliasFills"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
