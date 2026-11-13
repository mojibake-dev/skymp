#pragma once
#include "PropertyBinding.h"

// mp.get(refrId, "bodyRest"): a corpse's rest as the server records it
// (thuum docs/verbs/npc-fate.md "The body's rest", ADR-067 decision 4):
// {atRest: the owner's ragdoll came to rest or the settle window ended,
// restAt: the server's monotonic ms clock then (TickDeaths' clock), 0 while
// not at rest, owner: the form id of the game whose live row of the prop
// table owns the ragdoll (physics rung 5a, PartOne::PropLiveOwner; 0 none,
// the cell's host by default), samples: the owner's accepted samples};
// read-only, for the lab's ref read (labState kind ref carries atRest and
// restAt as bodyAtRest and bodyRestAt). null for a form that is no actor.
class BodyRestBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "bodyRest"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
