#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include "ScriptStateTypes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

// thuum ADR-068 rung 3 (the script record, decision 5 and its amendment A):
// a reference's recorded scripts served to a seat, right ahead of HostGrant
// to the seat about to host the reference's cell (ApplyHostDecisions), and
// right behind the reference's create word to a seat that does not host it
// (PartOne's onSubscribe: a late joiner, a restart), with the object's
// sequence when the record was taken and its view then, the reference's
// lastAnimation (none when no animation had reached it). Server to client,
// reliable, R0 output; never relayed live (MpObjectReference::
// BuildScriptStateApply). The receiver applies the scripts and replays the
// view where its own sequence for the object differs. The contract is
// skymp-wire's (wire-schema skymp.rs, ScriptStateApply, MsgType 77, wire id
// 85).
struct ScriptStateApplyMessage : public MessageBase<ScriptStateApplyMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::ScriptStateApply)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("formId", formId)
      .Serialize("fnv1a", fnv1a)
      .Serialize("seq", seq)
      .Serialize("event", event)
      .Serialize("scripts", scripts);
  }

  uint32_t formId = 0; // the reference
  uint32_t fnv1a = 0;  // the record's FNV-1a 32, the owner's
  uint32_t seq = 0;    // the object's sequence when the record was taken
  std::optional<std::string> event;       // the reference's view then
  std::vector<ScriptStateObject> scripts; // as the owner read them
};
