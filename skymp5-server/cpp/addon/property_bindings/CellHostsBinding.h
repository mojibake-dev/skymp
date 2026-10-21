#pragma once
#include "PropertyBinding.h"

// mp.get(0, "cellHosts"): every cell the server's host table knows (thuum
// docs/verbs/cell-host.md), each {cell, hosted, player, epoch}, read-only;
// the lab reads it through the gamemode's labState.
class CellHostsBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "cellHosts"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
