#pragma once
#include "ActorValues.h"
#include "libespm/Loader.h"
#include "libespm/espm.h"
#include <WorldState.h>
#include <cstdint>
#include <fmt/format.h>

#include "CreateActorMessage.h"

struct BaseActorValues : public ActorValues
{
  void VisitBaseActorValuesAndPercentages(BaseActorValues& baseActorValues,
                                          MpChangeForm& changeForm,
                                          CreateActorMessage& message);
};

// `playerLevel`: the player level a PC-level-mult NPC's level is computed
// from, the party level the game running the actor is told and its engine
// reads in place of its own player's (1 where none is known)
BaseActorValues GetBaseActorValues(WorldState* worldState, uint32_t baseId,
                                   uint32_t raceIdOverride,
                                   const std::vector<FormDesc>& templateChain,
                                   uint16_t playerLevel = 1);
