#pragma once
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

// A global variable (GLOB): its type (FNAM, one byte: 's' short, 'l' long,
// 'f' float) and the value the records hold before any save (FLTV, a float
// whatever the type). The server reads it for a condition's GetGlobalValue
// (thuum docs/DECISIONS.md ADR-039) when no player's own recorded global
// names it (docs/verbs/player-globals.md). UESP "Skyrim Mod:Mod File
// Format/GLOB".
class GLOB final : public RecordHeader
{
public:
  static constexpr auto kType = "GLOB";

  struct Data
  {
    char type = 'f';
    float value = 0.f;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(GLOB) == sizeof(RecordHeader));

}

#pragma pack(pop)
