#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// A look asked of a player's own game: whether that player sees `target`,
// which only its own engine can tell (its line-of-sight test for anyone but
// its own player refuses a target that is no actor). Client to server from
// the seat whose script asked it on the player's figure; server to client to
// that player's game alone (ActionListener::OnPlayerLookAsk), which watches
// its own player's look and answers with PlayerLookMessage. Both directions,
// reliable. The contract is skymp-wire's (wire-schema skymp.rs,
// PlayerLookAsk, MsgType 88, wire id 96).
struct PlayerLookAskMessage : public MessageBase<PlayerLookAskMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PlayerLookAsk)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("player", player)
      .Serialize("target", target);
  }

  uint32_t player = 0; // the looking player's actor, by its server id
  uint32_t target = 0; // the reference looked at, a load-order form
};
