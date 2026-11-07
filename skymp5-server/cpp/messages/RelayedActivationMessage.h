#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <type_traits>

// thuum docs/verbs/traps.md (M4): a linked activation the cell's host reported
// (a pressure plate firing its spike wall, a lever opening its portcullis),
// told to the cell's other games so each engine plays it with the caster the
// host's did (ActionListener::OnLinkedActivation). An actor target (ADR-068)
// is never told to the cell's other games: it goes to the one other seat
// that simulates the actor, or nowhere. Server to client,
// reliable. The contract is skymp-wire's (wire-schema skymp.rs,
// RelayedActivation, MsgType 70, wire id 78).
// Since SCHEMA_VERSION 40 (thuum D-C2): with `entry` the relay is a trigger
// entry a guest's game reported (ActionListener::OnTriggerEntry), sent to
// the cell's host alone, whose engine sends that event to the target's
// scripts with the caster as akActionRef instead of activating.
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
      .Serialize("caster", caster)
      .Serialize("entry", entry);
  }

  uint32_t target = 0; // the activated reference, never 0
  uint32_t caster = 0; // the reference that activated it, never 0
  // 1 OnTriggerEnter, 2 OnTriggerLeave (a u8 on the wire); absent on an
  // activation
  std::optional<uint32_t> entry;
};
