#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/npc-stream.md (the voice slice, ADR-050): a spoken line
// begins on an actor the sender's game simulates: the actor's idx, the
// topic's form id and the topic info's (the line). Client to server,
// unreliable; the server gates the host and relays the bytes to the actor's
// other listeners (ActionListener::OnActorSpeech), whose puppets say the
// same info through the engine's own say. The contract is skymp-wire's
// (wire-schema skymp.rs, ActorSpeech, MsgType 54, wire id 62).
struct ActorSpeechMessage : public MessageBase<ActorSpeechMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ActorSpeech)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("topic", topic)
      .Serialize("info", info);
  }

  uint32_t idx = 0;
  uint32_t topic = 0;
  uint32_t info = 0;
};
