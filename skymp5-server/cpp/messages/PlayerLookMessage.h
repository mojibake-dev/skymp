#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// A player's own game's answer to a look asked of it (PlayerLookAskMessage):
// whether its player sees `target` now, by its own engine's test. Client to
// server from that player's game, at a watch's first read and each change;
// server to client to the seat that hosts the target's cell, whose script
// asks (ActionListener::OnPlayerLook). Both directions, reliable. The
// contract is skymp-wire's (wire-schema skymp.rs, PlayerLook, MsgType 89,
// wire id 97).
struct PlayerLookMessage : public MessageBase<PlayerLookMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PlayerLook)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("player", player)
      .Serialize("target", target)
      .Serialize("los", los);
  }

  uint32_t player = 0; // the looking player's actor, by its server id
  uint32_t target = 0; // the reference looked at, a load-order form
  bool los = false;    // whether the player sees it
};
