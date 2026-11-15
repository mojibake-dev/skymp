#pragma once
#include "RecordHeader.h"
#include <cstdint>

#pragma pack(push, 1)

namespace espm {

// thuum ADR-073 step 4's S6 (Eli's D-NT4 A): a scene, whose VMAD attaches its
// fragment script and whose quest is its PNAM, one form id (UESP, "Skyrim
// Mod:Mod File Format/SCEN"; Skyrim.esm's MS13CamillaStage10Scene 0x383a3
// attaches SF_MS13CamillaStage10Scene_000383A3 and names MS13 0x39645 there,
// lab/esm.py, 2026-11-15)
class SCEN final : public RecordHeader
{
public:
  static constexpr auto kType = "SCEN";

  struct Data
  {
    // the scene's quest, this file's id; 0 for none
    uint32_t quest = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(SCEN) == sizeof(RecordHeader));

}

#pragma pack(pop)
