#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include "ScriptStateTypes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

// thuum ADR-068 rung 3 (the script record, decision 5 and its amendment A;
// docs/research/rung3-script-record-2026-11-06.md section 1(b)): the scripts
// attached to one reference as its owner's engine holds them, read at a
// quiescent point of the object (the platform's SerializeScriptState).
// Client to server, reliable, R2: the reference's owner, the host of its
// cell, reports a record when its hash moved, and at its release of the cell
// at the epoch it released; the wire validated the shape (wire-validate
// check_script_state: 1 to 16 scripts, each read at a quiescent point, every
// variable's kind and payload), the server judges the epoch and the owner
// (wire-rules script_state, ActionListener::OnScriptState) and keeps the
// last record on the reference, served as ScriptStateApply, at once to the
// reference's other listeners only when its graph state moved. Since thuum
// docs/verbs/traps.md "The graph state": `graph`, the reference's animation
// graph as its owner's engine saved it (the platform's SerializeGraphState,
// the engine's frame as lowercase hex; wire-validate check_graph_state holds
// the frame), absent when the engine gave none, in which case the server
// keeps the graph state the record it replaces carried. The contract is
// skymp-wire's (wire-schema skymp.rs, ScriptState, MsgType 76, wire id 84).
struct ScriptStateMessage : public MessageBase<ScriptStateMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ScriptState)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("epoch", epoch)
      .Serialize("formId", formId)
      .Serialize("fnv1a", fnv1a)
      .Serialize("scripts", scripts)
      .Serialize("graph", graph);
  }

  uint32_t epoch = 0;  // the cell's host epoch the owner holds
  uint32_t formId = 0; // the reference, a game-file placement
  uint32_t fnv1a = 0;  // the owner's FNV-1a 32 over the record's dump
  std::vector<ScriptStateObject> scripts; // 1 to 16, as the owner read them
  // the reference's animation graph state as the owner's engine saved it
  // (lowercase hex of the engine's frame, at most 16384 characters); absent
  // when it gave none
  std::optional<std::string> graph;
};
