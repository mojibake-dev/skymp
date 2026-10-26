#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/npc-talk.md (ADR-052): a player asks for an NPC another
// player's game simulates, for a dialogue (claim true), or gives it back
// (claim false). Client to server, reliable, R0: the server grants when the
// claimant stands in the actor's cell within activation reach and the actor
// is an NPC, alive, not in combat and unclaimed
// (ActionListener::OnActorClaim); the word names the claimant as the simulator
// for the claim's lease. The contract is skymp-wire's (wire-schema skymp.rs,
// ActorClaim, MsgType 56, wire id 64).
struct ActorClaimMessage : public MessageBase<ActorClaimMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ActorClaim)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("claim", claim);
  }

  uint32_t idx = 0;
  bool claim = true;
};
