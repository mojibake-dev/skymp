#pragma once
#include "PropertyBinding.h"

// mp.get(0, "questStages"): the world's quest stages done as the server
// keeps them on its journal (ADR-056 decision 2): [{quest, stage}], the
// quest a form id of the running load order (a plugin the order lacks
// leaves its row out); read-only, for the lab's claims (labState's
// questStages, thuum ADR-068 rung 5)
class QuestStagesBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "questStages"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
