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
// requirement (GetBaseActorValue) that the Skills menu checks. The sections'
// own conditions (the entries' effects) are not read. The form ids are the
// file's own; a caller makes them global through its lookup result.
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
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(PERK) == sizeof(RecordHeader));

}

#pragma pack(pop)
