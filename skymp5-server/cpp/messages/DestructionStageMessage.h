#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
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
// Since SCHEMA_VERSION 39 (thuum ADR-068 rung 4, amendment B read 7) the
// same word is a reference's engine state from the server: `disabled` and
// `deleted` (the record a host's scripted Enable, Disable or Delete moved,
// reported as ScriptCall 75) with the object's sequence `seq`, relayed at
// once and served behind the create word; a word with neither is a
// destruction word alone, and a seat's report carries neither and seq 0
// (wire-validate refuses one from a client otherwise).
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
      .Serialize("served", served)
      .Serialize("disabled", disabled)
      .Serialize("deleted", deleted)
      .Serialize("seq", seq);
  }

  uint32_t refr = 0;      // the reference, a game-file placement, never 0
  uint32_t stage = 0;     // the destruction stage, 0 to 127 (a u8 on the wire)
  bool destroyed = false; // the reference is destroyed (a spent trap)
  bool served = false;    // the record served behind a create word
  // thuum ADR-068 rung 4: the ref state's words, the server's alone; absent
  // on a destruction word
  std::optional<bool> disabled;
  std::optional<bool> deleted;
  uint32_t seq = 0; // the object's sequence at the ref state's record
};
