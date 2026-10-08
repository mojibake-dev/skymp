#pragma once
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

class CELL final : public RecordHeader
{
public:
  static constexpr auto kType = "CELL";

  enum Flags
  {
    Interior = 0x0001,
    HasWater = 0x0002,
    NotCantTravelFromHere = 0x0004,
    NoLODWater = 0x0008,
    PublicArea = 0x0020,
    HandChanged = 0x0040,
    SnowSky = 0x0080,
    UseSkyLighting = 0x0100
  };

  struct Data
  {
    uint16_t flags = 0;
  };

  Data GetData(CompressedFieldsCache& cache) const noexcept;

  // An exterior cell's grid square (XCLC: x and y as int32; UESP, "Skyrim
  // Mod:Mod File Format/CELL"); false for a cell without one
  bool GetGrid(int32_t& outX, int32_t& outY,
               CompressedFieldsCache& cache) const noexcept;
};

}

#pragma pack(pop)
