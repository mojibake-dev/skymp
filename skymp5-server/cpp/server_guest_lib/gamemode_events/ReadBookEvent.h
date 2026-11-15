#pragma once

#include <cstdint>

#include "GameModeEvent.h"

class MpActor;

class ReadBookEvent : public GameModeEvent
{
public:
  // when a skill book's record goes to the reader's game (thuum
  // docs/verbs/leveling.md, skill books; docs/verbs/books.md, S2)
  enum class RecordSend
  {
    // a read from the inventory (MpActor::OnEquip): its game's engine raised
    // the skill before its report, so the record goes at once
    kNow,
    // a world book's read at the server's open (MpActor::ReadBookAtOpen):
    // its game's engine raises the skill after the open reply, so the record
    // waits for the open's end (MpActor::EndOpenBook)
    kAtOpenEnd
  };

  ReadBookEvent(MpActor* actor_, uint32_t baseId_,
                RecordSend recordSend_ = RecordSend::kNow);

  const char* GetName() const override;

  std::string GetArgumentsJsonArray() const override;

  bool SpellLearned() const;

  // true when the skill branch held its record's send (kAtOpenEnd) for the
  // caller
  bool RecordHeld() const;

private:
  void OnFireSuccess(WorldState* worldState) override;
  void OnFireBlocked(WorldState* worldState) override;

  // the record to the reader's game now, or held for the open's end
  void SendRecord();

  // event arguments
  MpActor* actor = nullptr;
  uint32_t baseId = 0;
  RecordSend recordSend = RecordSend::kNow;

  // OnFireSuccess-specific arguments
  // ...

  // Results
  bool spellLearned = false;
  bool recordHeld = false;
};
