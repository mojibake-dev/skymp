#pragma once
#include "RecordHeader.h"
#include <cstdint>

#pragma pack(push, 1)

namespace espm {

// thuum ADR-073 step 5a (D-53-4): an AI package. A quest's lead is the
// quest's own package running on one of its alias actors, and the package
// names its quest in QNAM, its owner quest (CommonLibSSE-NG
// include/RE/T/TESPackage.h:310, `ownerQuest`, "QNAM"). Skyrim.esm as read on
// sky-srv 2026-11-14: MS13CamillaTravelMarker00Package 0x2e4cc carries one
// QNAM, MS13 0x39645, between its PSDT and PKCU; MS13CamillaWorkTrader
// 0x63b2a carries none
class PACK final : public RecordHeader
{
public:
  static constexpr auto kType = "PACK";

  struct Data
  {
    // the owner quest (QNAM), this file's id; 0 for a package no quest owns
    uint32_t ownerQuest = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(PACK) == sizeof(RecordHeader));

}

#pragma pack(pop)
