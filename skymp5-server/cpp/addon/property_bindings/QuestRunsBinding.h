#pragma once
#include "PropertyBinding.h"

// mp.get(0, "questRuns"): the world's quest runs as the server keeps them on
// its journal (thuum ADR-068 rung 5, QuestStage 57's start and stop words):
// [{quest, running, seq}], the quest a form id of the running load order
// (a plugin the order lacks leaves its row out), running its last word a
// start, seq the record's sequence for it; read-only, for the lab's claims
// (labState's questRuns)
class QuestRunsBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "questRuns"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
