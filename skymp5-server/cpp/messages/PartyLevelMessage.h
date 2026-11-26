#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// The level a game's PC-level-mult NPCs scale to: the party's level of the
// players the game stands with (the mean of their levels leaned toward the
// highest by the server setting npcScaleLean, cut to a whole level), sent
// when it changes and at a login. The game's engine computes a
// PC-level-mult NPC's level and stats from it in place of its own player's
// level, so the engine and the server count the same health. Server to
// client, reliable, R0. The contract is skymp-wire's (wire-schema skymp.rs,
// PartyLevel, MsgType 90, wire id 98).
struct PartyLevelMessage : public MessageBase<PartyLevelMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PartyLevel)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("level", level);
  }

  uint16_t level = 1; // the party's level; at least 1
};
