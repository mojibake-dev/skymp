#pragma once
#include "RecordHeader.h"
#include <cstdint>

#pragma pack(push, 1)

namespace espm {

// A class record: its attribute weights (DATA, the engine's CLASS_DATA,
// CommonLibSSE-NG include/RE/T/TESClass.h:11-81; the weights at 0x20, one
// byte each for health, magicka and stamina), which share an auto-calc NPC's
// level points among the three. The class's skills, teaching and training
// are not read.
class CLAS final : public RecordHeader
{
public:
  static constexpr auto kType = "CLAS";

  struct Data
  {
    const char* editorId = "";
    bool hasWeights = false; // a DATA of at least 35 bytes
    uint8_t healthWeight = 0;
    uint8_t magickaWeight = 0;
    uint8_t staminaWeight = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(CLAS) == sizeof(RecordHeader));

}

#pragma pack(pop)
