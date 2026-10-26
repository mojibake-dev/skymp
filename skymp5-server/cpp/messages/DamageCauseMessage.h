#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
// the server's number for a hit on an NPC some game hosts, sent to that host
// as a cause instead of absolute values: the resulting health (absolute,
// never a delta), the aggressor's form id (0 for none) and whether the
// number killed through a headshot or a scripted kill. Server to the host,
// reliable, R0 output; the host's engine applies it through its own damage
// entry (Skyrim Platform damageActor), so it reacts, records the killer and
// dies by itself at zero, which it reports (ActorDeath). The contract is
// skymp-wire's (wire-schema skymp.rs, DamageCause, MsgType 55, wire id 63).
struct DamageCauseMessage : public MessageBase<DamageCauseMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::DamageCause)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("attacker", attacker)
      .Serialize("health", health)
      .Serialize("killMove", killMove);
  }

  uint32_t idx = 0;
  uint32_t attacker = 0;
  float health = 0.f;
  bool killMove = false;
};
