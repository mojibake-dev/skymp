#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum D-S3 (traps follow the interaction; ADR-068's DestructionStage row):
// a single-use result recorded once. Every seat runs the trap family itself,
// so one oil lamp's fall or one tripwire's snap happens on several engines;
// the seat whose engine reached a step first reports it (a destruction stage
// rise, or SetDestroyed), the server records the first report of each step
// (wire-rules destruction, ActionListener::OnDestructionStage: a reference
// it holds or loads, no actor, in a cell the reporter's game has loaded) and
// relays it to the reference's other listeners, and serves the record to a
// seat right after the reference's create word (a late joiner, a restart:
// served true). Both directions, reliable. The contract is skymp-wire's
// (wire-schema skymp.rs, DestructionStage, MsgType 78, wire id 86).
struct DestructionStageMessage : public MessageBase<DestructionStageMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::DestructionStage)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refr", refr)
      .Serialize("stage", stage)
      .Serialize("destroyed", destroyed)
      .Serialize("served", served);
  }

  uint32_t refr = 0;      // the reference, a game-file placement, never 0
  uint32_t stage = 0;     // the destruction stage, 0 to 127 (a u8 on the wire)
  bool destroyed = false; // the reference is destroyed (a spent trap)
  bool served = false;    // the record served behind a create word
};
