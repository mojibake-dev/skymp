#pragma once
#include "ScriptStateMessage.h" // ScriptStateObject
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// thuum ADR-068 rung 3 (the script record, decision 5 and its amendment A;
// docs/research/rung3-script-record-2026-11-06.md section 1(c)): the last
// record a reference's owner reported of the scripts attached to it, as the
// server keeps it on the reference's own change form
// (MpChangeForm::scriptState, rule 6): the scripts as the owner's engine read
// them at a quiescent point (ScriptStateMessage.h), their FNV-1a 32 as the
// owner computed it (its compare baseline; the server never recomputes it),
// who sent it at which host epoch and when (wall-clock ms), and what the
// server stamped at that moment: the object's sequence (wire-rules natives'
// seq_of; after a restart the record seeds it back, ScriptCalls::seed) and
// the reference's lastAnimation, its view (amendment A: the Temple pillar's
// OnCellLoad turns the graph only while doOnce is unset, so a re-sent load
// event renders nothing and the record carries the event itself). Stored
// raw, as ChangeFormRecord.h's rows are: the handles' form ids are the load
// order both ends share (the modlock, ADR-065 risk 2), so ADR-043's
// plugin-relative spelling would buy them nothing. Absent in older records
// and on references nobody reported, read as none: an optional key,
// formVersion kept (this fork's migration for a new field). Its field list
// is its JSON form, written and read by the archives (Appearance's round
// trip).
struct ScriptStateRecord
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("epoch", epoch)
      .Serialize("fnv1a", fnv1a)
      .Serialize("sender", sender)
      .Serialize("at", atMs)
      .Serialize("seq", seq)
      .Serialize("event", event)
      .Serialize("scripts", scripts);
  }

  uint32_t epoch = 0;  // the cell's host epoch the report named
  uint32_t fnv1a = 0;  // the owner's FNV-1a 32 over the scripts' dump
  uint32_t sender = 0; // the reporting host's actor
  uint64_t atMs = 0;   // wall-clock ms of the report
  uint32_t seq = 0;    // the object's sequence when the record was taken
  std::optional<std::string> event;       // the reference's lastAnimation
  std::vector<ScriptStateObject> scripts; // 1 to 16, as the owner read them

  auto ToTuple() const
  {
    return std::make_tuple(epoch, fnv1a, sender, atMs, seq, event, scripts);
  }
  friend bool operator==(const ScriptStateRecord& lhs,
                         const ScriptStateRecord& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  friend bool operator!=(const ScriptStateRecord& lhs,
                         const ScriptStateRecord& rhs)
  {
    return !(lhs == rhs);
  }
  friend bool operator<(const ScriptStateRecord& lhs,
                        const ScriptStateRecord& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
