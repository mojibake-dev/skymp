#pragma once
#include "PropertyBinding.h"

// mp.get(refrId, "scriptState"): the last script record a reference's owner
// reported, as the server keeps it (thuum ADR-068 rung 3, the script record):
// {fnv1a: the owner's FNV-1a 32, at: wall-clock ms of the report, epoch,
// sender: the reporting host's actor, seq: the object's sequence when the
// record was taken, event: its view then (absent for none), scripts: the
// scripts' count}; read-only, for the lab's claims (labState ref's
// scriptStateFnv and scriptStateAt). null for a form that is no reference or
// a reference nobody reported.
class ScriptStateBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "scriptState"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
