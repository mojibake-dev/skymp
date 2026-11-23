#pragma once
#include "RecordHeader.h"
#include <vector>

#pragma pack(push, 1)

namespace espm {

// A hazard: DATA's lifetime 0x08 (seconds) and the spell it casts on what
// stands in it 0x18 (UESP "Skyrim Mod:Mod File Format/HAZD", read from the
// lab's masters with thuum lab/esm.py: FireLgShortHazard 0x3E076 lasts 2 s
// and casts HazardFireSpell 0x153BD), 0 for none
class HAZD final : public RecordHeader
{
public:
  static constexpr auto kType = "HAZD";

  struct Data
  {
    float lifetime = 0.f;
    uint32_t spell = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(HAZD) == sizeof(RecordHeader));

}

#pragma pack(pop)
