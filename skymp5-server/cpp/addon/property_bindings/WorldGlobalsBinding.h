#pragma once
#include "PropertyBinding.h"

// mp.get(0, "worldGlobals"): the world's globals as the server keeps them on
// its journal and who streams them (thuum ADR-068 rung 4, WorldGlobal 80):
// {streamer: the streaming seat's actor (0 for none), epoch, entries:
// [{global, value}]}, each global a form id of the running load order (a
// recorded global whose plugin the order lacks is left out); read-only, for
// the lab's claims (labState's world_globals).
class WorldGlobalsBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "worldGlobals"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
