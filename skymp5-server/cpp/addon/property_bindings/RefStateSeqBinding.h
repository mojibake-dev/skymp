#pragma once
#include "PropertyBinding.h"

// mp.get(refrId, "refStateSeq"): the object's sequence at the reference's
// last recorded ref state (thuum ADR-068 rung 4: its owner's Enable, Disable
// or Delete, or the server's own), null while none is recorded; read-only,
// for the lab's claims (labState's ref refStateSeq)
class RefStateSeqBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "refStateSeq"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
