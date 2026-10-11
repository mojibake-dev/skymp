#pragma once
#include "RecordHeader.h"
#include <array>
#include <cstdint>

#pragma pack(push, 1)

namespace espm {

// A shout (thuum docs/verbs/shouts.md): its three variations, each a SNAM of
// the word of power, the spell cast and the recovery in seconds (UESP,
// "Skyrim Mod:Mod File Format/SHOU"; CommonLibSSE-NG TESShout::Variation).
// The form ids are the file's own; a caller makes them global through its
// lookup result.
class SHOU final : public RecordHeader
{
public:
  static constexpr auto kType = "SHOU";

  struct Variation
  {
    uint32_t word = 0;
    uint32_t spell = 0;
    float recovery = 0.f;
  };

  struct Data
  {
    std::array<Variation, 3> variations = {};
    uint32_t count = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(SHOU) == sizeof(RecordHeader));

}

#pragma pack(pop)
