#pragma once
#include "RecordHeader.h"
#include <cstdint>

#pragma pack(push, 1)

namespace espm {

// A climate record (thuum docs/verbs/weather.md): its timing (TNAM, the
// engine's TESClimate::timing, CommonLibSSE-NG include/RE/T/TESClimate.h:60,
// laid out :41-46: sunrise begin and end, sunset begin and end, volatility,
// moon phase length). The volatility sets how long the engine holds a
// weather before it draws the next. The weather list (WLST), the sky models
// and the textures are not read.
class CLMT final : public RecordHeader
{
public:
  static constexpr auto kType = "CLMT";

  struct Data
  {
    const char* editorId = "";
    bool hasTiming = false; // a TNAM of at least five bytes
    uint8_t volatility = 0; // TNAM's fifth byte
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(CLMT) == sizeof(RecordHeader));

}

#pragma pack(pop)
