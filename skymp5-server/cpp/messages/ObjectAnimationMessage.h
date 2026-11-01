#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <string>
#include <type_traits>

// thuum docs/verbs/traps.md (ADR-059 decision 3): a world object's animation
// event as the cell's host's engine played it (a puzzle pillar's Turned01, a
// lever's FullPull, a portcullis's Open), reported by the host
// (ActionListener::OnObjectAnimation), recorded as the reference's
// lastAnimation and told to the cell's other games, whose engines run no
// world script. Both directions, reliable. The contract is skymp-wire's
// (wire-schema skymp.rs, ObjectAnimation, MsgType 71, wire id 79).
struct ObjectAnimationMessage : public MessageBase<ObjectAnimationMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::ObjectAnimation)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refr", refr)
      .Serialize("event", event);
  }

  uint32_t refr = 0; // the animated reference, a game-file world object
  std::string event; // the behavior graph event name, never empty
};
