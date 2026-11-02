#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/traps.md (ADR-059 decision 4): a trap's hit on another
// player's body as the cell's host's engine dealt it (the swinging wall's
// leveled damage through the engine's ProcessTrapHit), reported by the host
// (ActionListener::OnTrapHit). The server checks the number against the
// trap's own table from the game files and the hit against the trap's reach
// (wire-rules traps), applies the table's value to the target's record and
// answers with ChangeValues. Client to server, reliable. The contract is
// skymp-wire's (wire-schema skymp.rs, TrapHit, MsgType 72, wire id 80).
struct TrapHitMessage : public MessageBase<TrapHitMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::TrapHit)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refr", refr)
      .Serialize("target", target)
      .Serialize("damage", damage);
  }

  uint32_t refr = 0;   // the trap reference, a game-file world object
  uint32_t target = 0; // the hit actor, another player's
  float damage = 0.f;  // the damage the host's engine dealt
};
