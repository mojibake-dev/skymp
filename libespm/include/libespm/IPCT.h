#pragma once
#include "RecordHeader.h"
#include <vector>

#pragma pack(push, 1)

namespace espm {

// An impact: the hazard (HAZD) it places, NAM2 (UESP "Skyrim Mod:Mod File
// Format/IPCT", read from the lab's masters with thuum lab/esm.py:
// MAGFlamesImpactLS01 0x3E077 names FireLgShortHazard 0x3E076), 0 for none
class IPCT final : public RecordHeader
{
public:
  static constexpr auto kType = "IPCT";

  struct Data
  {
    uint32_t hazard = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(IPCT) == sizeof(RecordHeader));

}

#pragma pack(pop)
