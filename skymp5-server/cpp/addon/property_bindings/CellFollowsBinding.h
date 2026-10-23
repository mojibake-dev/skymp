#pragma once
#include "PropertyBinding.h"

// mp.get(0, "cellFollows"): every follow the server's cell table holds
// (thuum docs/verbs/npc-handoff.md D6, ADR-047), each {actor, player, cell}:
// an actor a host's game simulates in a cell nobody hosts; read-only, the
// lab reads it through the gamemode's labState as it reads cellHosts.
class CellFollowsBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "cellFollows"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
