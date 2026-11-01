#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/traps.md (M4): a linked activation the cell's host reported
// (a pressure plate firing its spike wall, a lever opening its portcullis),
// told to the cell's other games so each engine plays it with the caster the
// host's did (ActionListener::OnLinkedActivation). Server to client,
// reliable. The contract is skymp-wire's (wire-schema skymp.rs,
// RelayedActivation, MsgType 70, wire id 78).
struct RelayedActivationMessage : public MessageBase<RelayedActivationMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::RelayedActivation)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("target", target)
      .Serialize("caster", caster);
  }

  uint32_t target = 0; // the activated reference, never 0
  uint32_t caster = 0; // the reference that activated it, never 0
};
