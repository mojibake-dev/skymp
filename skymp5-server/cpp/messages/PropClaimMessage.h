#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/prop-physics.md (ADR-058 decision 1): a player's game
// touched a prop it does not simulate. Client to server, reliable, R0: the
// server names the newest toucher the simulator (one epoch per prop) and
// tells every listener of the prop (PropOwner); refused when the claimant is
// not in the prop's cell or another player holds the prop grabbed
// (ActionListener::OnPropClaim, wire-rules props). Touches: 0 a contact of
// the character controller, 1 a grab, 2 a drop, 3 a shout's or spell's
// impulse (wire-validate's bound). The contract is skymp-wire's (wire-schema
// skymp.rs, PropClaim, MsgType 59, wire id 67).
struct PropClaimMessage : public MessageBase<PropClaimMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PropClaim)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refr", refr)
      .Serialize("touch", touch);
  }

  uint32_t refr = 0;  // the prop's reference form id, never 0
  uint32_t touch = 0; // 0 to 3 (a u8 on the wire; ActorDeath's cause idiom)
};
