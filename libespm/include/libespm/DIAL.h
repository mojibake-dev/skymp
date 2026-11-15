#pragma once
#include "RecordHeader.h"
#include <cstdint>

#pragma pack(push, 1)

namespace espm {

// thuum ADR-073 step 4's S6 (Eli's D-NT4 A): a dialogue topic, whose quest is
// its QNAM, one form id (UESP, "Skyrim Mod:Mod File Format/DIAL"; Skyrim.esm's
// DA01IntroRumorsTopic 0x24390 names DA01Intro 0x92e25 there, lab/esm.py,
// 2026-11-15). Its topic infos are the records of the group that follows it,
// a TOPIC_CHILDREN group labelled with its id (GroupHeader::GetParentDIAL)
class DIAL final : public RecordHeader
{
public:
  static constexpr auto kType = "DIAL";

  struct Data
  {
    // the topic's quest, this file's id; 0 for none
    uint32_t quest = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(DIAL) == sizeof(RecordHeader));

}

#pragma pack(pop)
