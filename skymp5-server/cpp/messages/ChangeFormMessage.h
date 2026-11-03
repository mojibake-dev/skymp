#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// thuum docs/verbs/change-form.md (ADR-065 system 1, D6): one remap row of a
// packed record: where a kind-0 id (an index into the SENDER's form-id map)
// sits in the body, the form it stands for in the load order both ends
// share, and the index the bytes decode to. The receiver re-encodes the form
// through its own engine; the server stores the rows untranslated.
struct ChangeFormRemapRow
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("offset", offset)
      .Serialize("formId", formId)
      .Serialize("index", index);
  }
  auto ToTuple() const { return std::make_tuple(offset, formId, index); }
  friend bool operator==(const ChangeFormRemapRow& lhs,
                         const ChangeFormRemapRow& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  friend bool operator<(const ChangeFormRemapRow& lhs,
                        const ChangeFormRemapRow& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
  uint32_t offset = 0; // the byte offset of the three-byte id in the body
  uint32_t formId = 0; // the form it names, never 0, never 00xxxxxx or ff
  uint32_t index = 0;  // the 22-bit index the bytes decode to on the sender
};

// thuum docs/verbs/change-form.md (ADR-065 system 1): an actor's change form
// as the owner's engine serialized it at the frame's end (the pack's record:
// the type and size-code byte, the version 0x4E, two lengths, the data).
// Client to server, reliable, R2: the cell's host reports the actors it
// simulates; the wire validated the shape (the header against the body, the
// hash, the remap rows: wire-validate check_change_form), the server judges
// the owner and the epoch (ActionListener::OnChangeForm), records the body,
// acks its hash (ChangeFormAck) and serves it (ChangeFormApply). The contract
// is skymp-wire's (wire-schema skymp.rs, ChangeForm, MsgType 72, wire id 80).
struct ChangeFormMessage : public MessageBase<ChangeFormMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ChangeForm)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("epoch", epoch)
      .Serialize("formId", formId)
      .Serialize("formType", formType)
      .Serialize("flags", flags)
      .Serialize("version", version)
      .Serialize("body", body)
      .Serialize("fnv1a", fnv1a)
      .Serialize("remap", remap);
  }

  uint32_t epoch = 0;    // the cell's host epoch the owner holds
  uint32_t formId = 0;   // the actor, a game-file id
  uint32_t formType = 0; // the record's save-type index (0 REFR, 1 ACHR)
  uint32_t flags = 0;    // the change flags the owner's engine marked
  uint32_t version = 0;  // the form version byte, 0x4E
  std::string body;      // the whole packed record, two lowercase hex
                         // characters a byte, as the native spelled it
  uint32_t fnv1a = 0;    // FNV-1a 32 over the record's bytes
  std::vector<ChangeFormRemapRow> remap; // the sender's census (D6)
};
