#pragma once
#include "PropertyBinding.h"

// mp.get(refrId, "changeFormRecord"): the latest change form an actor's owner
// reported, as the server keeps it (thuum docs/verbs/change-form.md,
// ADR-065): {fnv1a: the record's FNV-1a 32, at: wall-clock ms of the report,
// epoch, flags, formType, version, sender: the reporting host's actor,
// bodyLength: the record's size in bytes, remapRows: the census rows'
// count}; read-only, for the lab's claims (labState ref's changeFormFnv and
// changeFormAt). null for a form that is no actor or an actor nobody
// reported.
class ChangeFormRecordBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "changeFormRecord"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
