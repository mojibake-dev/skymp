#pragma once
#include "PropertyBinding.h"

// thuum docs/verbs/npc-fate.md (the lab's respawn-ref without inPlace): a dead
// NPC up again at its placement by the fate's own path, MpActor::RespawnFated
// (RebuildAndRespawn with the teleport, the roll anew), the shape the sweep
// sends (the death-state container with tTeleport), so a probe reads the same
// wire the sweep produces. Write-only: set true to respawn; reading answers
// whether the actor is dead (so a script can tell what a write would do).
class RespawnFatedBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "respawnFated"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
  void Set(Napi::Env env, ScampServer& scampServer, uint32_t formId,
           Napi::Value newValue) override;
};
