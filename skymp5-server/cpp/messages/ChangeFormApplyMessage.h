#pragma once
#include "ChangeFormMessage.h" // ChangeFormRemapRow
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

// thuum docs/verbs/change-form.md (ADR-065 system 1): a recorded change form
// served to a seat that sees the actor, before the actor's first frame
// there: after the CreateActor word on the same ordered channel, at a cell's
// load for the cell's actors with a record, and at once to a seat already
// listening when the record arrives. The remap rows are the sender's,
// untranslated: the receiver re-encodes them through its own engine. Server
// to client, reliable, R0 output (MpActor::SendChangeFormRecordTo). The
// contract is skymp-wire's (wire-schema skymp.rs, ChangeFormApply, MsgType
// 74, wire id 82).
struct ChangeFormApplyMessage : public MessageBase<ChangeFormApplyMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::ChangeFormApply)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("formId", formId)
      .Serialize("formType", formType)
      .Serialize("flags", flags)
      .Serialize("version", version)
      .Serialize("body", body)
      .Serialize("remap", remap);
  }

  uint32_t formId = 0;
  uint32_t formType = 0;
  uint32_t flags = 0;
  uint32_t version = 0;
  std::string body; // the sender's hex, unchanged
  std::vector<ChangeFormRemapRow> remap;
};
