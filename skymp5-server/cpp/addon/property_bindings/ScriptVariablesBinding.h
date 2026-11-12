#pragma once
#include "PropertyBinding.h"

// mp.get(0, "scriptVariables"): the world's condition-read script variables
// as the server keeps them on its journal (thuum ADR-068 rung 5,
// ScriptVariables 81, D-V1 B: the latest write per key): [{form, script,
// var, k, i | f | b, seq}], the form a form id of the running load order (a
// plugin the order lacks leaves its row out), the value the one field its
// kind names (3 i, 4 f, 5 b), seq the record's sequence for the key; a
// player's own keys live on its actor and are not listed; read-only, for the
// lab's claims (labState's scriptVariables)
class ScriptVariablesBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "scriptVariables"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
