#pragma once
#include "PropertyBinding.h"

// mp.get(0, "streamCounters"): what each connection's stream did (thuum
// docs/verbs/npc-stream.md, slice 4, ADR-050 decision 5), one row per user
// and SkyMP message type: {user, player, t, accepted, dropped, foreign};
// read-only, the lab reads it through the gamemode's labState.
class StreamCountersBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "streamCounters"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
