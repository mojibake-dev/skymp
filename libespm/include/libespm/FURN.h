#pragma once
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

// furniture
class FURN final : public RecordHeader
{
public:
  static constexpr auto kType = "FURN";

  // MNAM, the furniture's active markers and flags. The engine lets the
  // player sleep in furniture with this bit (CommonLibSSE-NG
  // include/RE/T/TESFurniture.h:50, TESFurniture::ActiveMarker::kCanSleep):
  // Skyrim.esm's beds, bedrolls and cots carry it, its chairs do not (thuum
  // lab/esm.py, 2026-10-04; docs/verbs/sleep.md)
  static constexpr uint32_t kCanSleep = 1u << 31;

  struct Data
  {
    uint32_t activeMarkers = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(FURN) == sizeof(RecordHeader));

}

#pragma pack(pop)
