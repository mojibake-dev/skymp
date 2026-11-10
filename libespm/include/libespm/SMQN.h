#pragma once
#include "RecordHeader.h"
#include <cstdint>
#include <vector>

#pragma pack(push, 1)

namespace espm {

// thuum ADR-068 rung 5 (D-Q4 C): a Story Manager quest node, the node whose
// quests the Story Manager starts at an event (A Lovely Letter's
// FreeformRiverwoodNode 0xb555d at a Change Location into Riverwood). Its
// quests are its NNAM fields, one form id each (CommonLibSSE-NG
// include/RE/B/BGSStoryManagerQuestNode.h:48, `quests`, "NNAM"; Skyrim.esm's
// nodes read field by field in thuum docs/research/m3/re-story-location.md,
// QNAM the count and one NNAM per quest)
class SMQN final : public RecordHeader
{
public:
  static constexpr auto kType = "SMQN";

  struct Data
  {
    // the quests the node names, in the record's order, this file's ids
    std::vector<uint32_t> quests;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(SMQN) == sizeof(RecordHeader));

}

#pragma pack(pop)
