#pragma once
#include "PropertyBinding.h"

// mp.get(refrId, "actorFate"): a game-file actor's fate as the server rolled
// it (thuum docs/verbs/npc-fate.md, ADR-049): {baseId: the NPC_ an engine's
// leveled roll is pinned to, level: the roll's level (0: the old rule),
// raceId: that NPC_'s race from the game files (the species a seat shows),
// worn: the outfit roll's items by form id}; read-only, for the lab's parity
// read of both seats against the record. null for a form that is no actor.
class ActorFateBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "actorFate"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
};
