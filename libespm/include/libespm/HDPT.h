#pragma once
#include "RecordHeader.h"
#include <vector>

#pragma pack(push, 1)

namespace espm {

// A head part (UESP, "Skyrim Mod:Mod File Format/HDPT"; the fields as thuum
// read them from rotfern.esp on 2026-10-08: DATA its flags, PNAM its type,
// HNAM each extra part it brings, RNAM the form list of races that may wear
// it). Form ids are raw, as the record's own file numbers them.
class HDPT final : public RecordHeader
{
public:
  static constexpr auto kType = "HDPT";

  enum Flags : uint8_t
  {
    kPlayable = 0x01,
    kMale = 0x02,
    kFemale = 0x04,
  };

  struct Data
  {
    uint8_t flags = 0;
    uint32_t type = 0;
    std::vector<uint32_t> extraParts;
    uint32_t validRaces = 0; // a FLST; 0 when the record lists none
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(HDPT) == sizeof(RecordHeader));

}

#pragma pack(pop)
