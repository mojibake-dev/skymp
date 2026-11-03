#pragma once
#include "PropertyBinding.h"

// thuum docs/verbs/npc-fate.md (the lab's respawn-ref without inPlace): a dead
// NPC up again at its placement by the fate's own path, MpActor::RespawnFated
// (RebuildAndRespawn with the teleport, the roll anew), the shape the sweep
// sends (the death-state container with tTeleport), so a probe reads the same
// wire the sweep produces. The write is the lever (true; anything else
// throws); the read answers whether the actor is dead (so a script can tell
// what a write would do). The lever skips the sweep's eligibility (a unique
// base keeps its body, a disabled body, placedDead): a lab lever's point. A
// player's actor is refused (IsFateRespawned is every non-player actor): the
// players' respawn is their own rule (the fresh review of b64d1bad, notes 1,
// 2, 6 and 8).
class RespawnFatedBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "respawnFated"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
  void Set(Napi::Env env, ScampServer& scampServer, uint32_t formId,
           Napi::Value newValue) override;
};
