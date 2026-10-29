#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/prop-physics.md (ADR-058 decision 1): who simulates a
// prop from now, to every listener of the prop at each owner change. Server
// to client, reliable, R0 output: the named player's seat makes its body
// Dynamic with the last streamed velocity, every other seat's goes
// Keyframed; a word with an epoch below one already applied is dropped (a
// fencing token, one counter per prop). The contract is skymp-wire's
// (wire-schema skymp.rs, PropOwner, MsgType 60, wire id 68).
struct PropOwnerMessage : public MessageBase<PropOwnerMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PropOwner)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refr", refr)
      .Serialize("player", player)
      .Serialize("epoch", epoch);
  }

  uint32_t refr = 0;   // the prop's reference form id, never 0
  uint32_t player = 0; // the simulating player's actor form id, never 0
  uint32_t epoch = 0;  // the prop's owner epoch, up at every grant
};
