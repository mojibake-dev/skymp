#pragma once
#include "RecordHeader.h"
#include <cstdint>
#include <vector>

#pragma pack(push, 1)

namespace espm {

// A region record (thuum docs/verbs/weather.md): the worldspace it belongs
// to (WNAM) and its weather list (RDWT, one 12-byte entry per weather: the
// WTHR form id, its chance in percent and an unused global; UESP, "Skyrim
// Mod:Mod File Format/REGN"). Skyrim's sky is drawn from these lists, not
// from the climate's (SkyrimClimate names one weather); the server pools a
// worldspace's regions into its weather pool. The form ids are the file's
// own; a caller makes them global through its lookup result. The region's
// polygons, objects, sounds and map name are not read.
class REGN final : public RecordHeader
{
public:
  static constexpr auto kType = "REGN";

  struct WeatherEntry
  {
    uint32_t weather = 0; // WTHR
    uint32_t chance = 0;  // percent
    uint32_t global = 0;  // GLOB, unused by the game
  };

  struct Data
  {
    const char* editorId = "";
    uint32_t worldspace = 0; // WNAM
    std::vector<WeatherEntry> weathers;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(REGN) == sizeof(RecordHeader));

}

#pragma pack(pop)
