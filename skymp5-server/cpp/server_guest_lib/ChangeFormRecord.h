#pragma once
#include "ChangeFormMessage.h" // ChangeFormRemapRow
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

// thuum docs/verbs/change-form.md (ADR-065 system 1): the latest change form
// an actor's owner reported, as the server keeps it on the actor's own change
// form (MpChangeForm::changeFormRecord, rule 6): the whole packed record as
// the sender's hex (two lowercase characters a byte, never rewritten: the
// receiver re-encodes its type-0 ids from the rows itself), the hash the wire
// checked, the sender's remap rows untranslated, who sent it at which host
// epoch and when (wall-clock ms). Stored raw: the record's own ids are bound
// to the load order it was written under already (a type-1 id carries its
// load index), so the rows' form ids keep the same binding instead of
// ADR-043's plugin-relative spelling. Absent in older records and on actors
// nobody reported, read as none: an optional key, formVersion kept (this
// fork's migration for a new field).
struct ChangeFormRecord
{
  uint32_t epoch = 0;   // the cell's host epoch the report named
  uint32_t flags = 0;   // the change flags the owner's engine marked
  uint8_t version = 0;  // the form version byte, 0x4E on 1.6.1170
  uint8_t formType = 0; // the save-type index (0 REFR, 1 ACHR)
  std::string bodyHex;  // the whole packed record, as received
  uint32_t fnv1a = 0;   // FNV-1a 32 over the record's bytes
  std::vector<ChangeFormRemapRow> remap; // the sender's census (D6)
  uint32_t sender = 0;                   // the reporting host's actor
  uint64_t atMs = 0;                     // wall-clock ms of the report

  // the record's size in bytes (the hex holds two characters a byte)
  size_t BodyLength() const { return bodyHex.size() / 2; }

  auto ToTuple() const
  {
    return std::make_tuple(epoch, flags, version, formType, bodyHex, fnv1a,
                           remap, sender, atMs);
  }
  friend bool operator==(const ChangeFormRecord& lhs,
                         const ChangeFormRecord& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  friend bool operator!=(const ChangeFormRecord& lhs,
                         const ChangeFormRecord& rhs)
  {
    return !(lhs == rhs);
  }
  friend bool operator<(const ChangeFormRecord& lhs,
                        const ChangeFormRecord& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
