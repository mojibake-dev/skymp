#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/change-form.md (ADR-065 system 1): the server's receipt of
// a recorded change form, the hash the owner compares its next serialization
// against (equal bytes send nothing, amendment A). Server to client,
// reliable, R0, to the reporting host alone. The contract is skymp-wire's
// (wire-schema skymp.rs, ChangeFormAck, MsgType 73, wire id 81).
struct ChangeFormAckMessage : public MessageBase<ChangeFormAckMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ChangeFormAck)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("formId", formId)
      .Serialize("fnv1a", fnv1a);
  }

  uint32_t formId = 0; // the actor
  uint32_t fnv1a = 0;  // the recorded body's FNV-1a 32
};
