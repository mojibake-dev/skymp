#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/npc-stream.md (ADR-050 decision 1, slice 2): a host's
// state for an actor it simulates, as its engine has it: the three
// percentages, its combat target as the server knows it (0 for none) and
// the package its last package event named (0 for none). Client to server,
// under the actor's idx, unreliable (the first for an actor after a grant
// reliable), on a change at most twice a second. The server judges the
// values against its own numbers (wire-rules hosted_state) and records the
// rest; its output is ChangeValues to the actor's other listeners
// (ActionListener::OnHostedActorState). The contract is skymp-wire's
// (wire-schema skymp.rs, HostedActorState, MsgType 53, wire id 61).
struct HostedActorStateMessage : public MessageBase<HostedActorStateMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::HostedActorState)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("health", health)
      .Serialize("magicka", magicka)
      .Serialize("stamina", stamina)
      .Serialize("combatTarget", combatTarget)
      .Serialize("package", package)
      .Serialize("gazeTarget", gazeTarget)
      .Serialize("gazeSlot", gazeSlot)
      .Serialize("emotionType", emotionType)
      .Serialize("emotionValue", emotionValue);
  }

  uint32_t idx = 0;
  float health = 1.f;
  float magicka = 1.f;
  float stamina = 1.f;
  uint32_t combatTarget = 0;
  uint32_t package = 0;
  // slice 5 (the gaze and the face): the head-track target as the server
  // knows it (0 none), the engine's slot type (below 6), the emotion type
  // (at most 7) and strength (at most 100); the server records them and
  // relays the message to the actor's other listeners, whose puppets apply
  // them (the percentages stay the server's ChangeValues)
  uint32_t gazeTarget = 0;
  uint32_t gazeSlot = 0;
  uint32_t emotionType = 0;
  uint32_t emotionValue = 0;
};
