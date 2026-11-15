#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum docs/verbs/prop-physics.md (ADR-058 decision 1; A2 and A3 of ADR-071
// decision 2; physics rung 2's v2, ADR-071 decision 6): a player's game
// touched props it does not simulate, one to sixteen in one message (an
// impact's bodies), each judged alone with an outcome of its own. Client to
// server, reliable, R0: the server arbitrates: a prop at rest or stalled is
// the newest toucher's (one epoch per prop) and every listener hears the word
// (PropOwner); a prop another seat streams awake is refused to a contact, a
// drop or an impulse (E_PROP_MOVING), a grab takes it; a grabbed prop
// refuses everything (E_PROP_HELD); a claimant that is no listener of the
// prop is refused (E_PROP_UNSEEN); the claimant's cell and distance are no
// gate (ActionListener::OnPropClaim, one reference at a time, wire-rules
// props). Touches: 0 a contact of the character controller, 1 a grab, 2 a
// drop, 3 a shout's or spell's impulse (wire-validate's bound, with the
// references' one to sixteen, none 0, each once). The contract is
// skymp-wire's (wire-schema skymp.rs, PropClaim, MsgType 59, wire id 67).
struct PropClaimMessage : public MessageBase<PropClaimMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PropClaim)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refrs", refrs)
      .Serialize("touch", touch);
  }

  // the props' reference form ids, one to sixteen, none 0, each once
  std::vector<uint32_t> refrs;
  uint32_t touch = 0; // 0 to 3 (a u8 on the wire; ActorDeath's cause idiom)
};
