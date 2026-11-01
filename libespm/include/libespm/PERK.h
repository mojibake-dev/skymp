#pragma once
#include "CTDA.h"
#include "RecordHeader.h"
#include <cstdint>
#include <vector>

#pragma pack(push, 1)

namespace espm {

// A perk record (thuum docs/verbs/perks.md): DATA's five bytes (isTrait,
// level, numRanks, isPlayable, isHidden), NNAM the next rank's perk, and the
// conditions BEFORE the first PRKE section, which UESP calls the "condition
// data subrecords for the perk to be available to the player" ("Skyrim
// Mod:Mod File Format/PERK"): the tree's lines (HasPerk) and the skill's
// requirement (GetBaseActorValue) that the Skills menu checks. Then the
// sections, one Entry each: PRKE (type, rank, priority), for an entry point
// (type 2) DATA (entry point, function, tab count), PRKC naming the tab the
// CTDAs after it belong to, EPFT the data type and EPFD the value (one float
// for type 1), PRKF closing the entry (the same UESP page; thuum
// docs/verbs/perks.md, the second half). The form ids are the file's own; a
// caller makes them global through its lookup result.
class PERK final : public RecordHeader
{
public:
  static constexpr auto kType = "PERK";

  struct Data
  {
    const char* editorId = "";
    bool isTrait = false;
    uint8_t level = 0;
    uint8_t numRanks = 0;
    bool isPlayable = false;
    bool isHidden = false;
    uint32_t nextPerk = 0; // NNAM, 0 for the last rank
    std::vector<CTDA> conditions;
    struct Entry
    {
      uint8_t type = 0; // PRKE: 0 a quest, 1 an ability, 2 an entry point
      uint8_t rank = 0;
      uint8_t priority = 0;
      uint8_t entryPoint = 0; // DATA of three bytes, type 2 only
      uint8_t function = 0;
      uint8_t tabCount = 0;
      uint8_t dataType = 0; // EPFT: 1 one float in EPFD
      float value = 0.f;    // EPFD's first float
      // (the tab PRKC named, the condition), in the file's order
      std::vector<std::pair<uint8_t, CTDA>> conditions;
    };
    std::vector<Entry> entries;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(PERK) == sizeof(RecordHeader));

}

#pragma pack(pop)
