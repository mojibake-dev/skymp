#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/prop-physics.md (ADR-058 decision 1; A1 to A7 of ADR-071
// decision 2): who simulates a prop from now, to every listener of the prop
// at each owner change: a grant, the return to the cell's host at any
// owner's own asleep sample (the host's and a holder's included), a refused
// final sample, the host's own wake of a resting prop (the word names the
// host by id), a departure, the simulator's unload of the prop's cell; to
// one seat alone as the answer to its refused claim or sample, and to a late
// listener after the prop's create word. Server to client, reliable, R0
// output: the named player's seat simulates (its body Dynamic), every other
// seat's follows the stream; player 0 names the host of the cell the prop's
// record stands in; a word with an epoch below one already applied is
// dropped and an equal one replaces (a fencing token, one counter per prop).
// The contract is skymp-wire's (wire-schema skymp.rs, PropOwner, MsgType 60,
// wire id 68).
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
  uint32_t player = 0; // the simulating player's actor form id, 0 the host
  uint32_t epoch = 0;  // the prop's owner epoch, up at every word
};
