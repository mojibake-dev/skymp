#pragma once
#include "RecordHeader.h"
#include <vector>

#pragma pack(push, 1)

namespace espm {

// An impact data set: the impact (IPCT) a hit places on each material, one
// PNAM a pair (material 0x00, impact 0x04; UESP "Skyrim Mod:Mod File
// Format/IPDS", read from the lab's masters with thuum lab/esm.py:
// MAGFlames01ImpactSet 0x26113's pairs name MAGFlamesImpact* impacts)
class IPDS final : public RecordHeader
{
public:
  static constexpr auto kType = "IPDS";

  struct Data
  {
    std::vector<uint32_t> impacts;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(IPDS) == sizeof(RecordHeader));

}

#pragma pack(pop)
