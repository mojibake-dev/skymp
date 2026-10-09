#pragma once
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

class AMMO final : public RecordHeader
{
public:
  static constexpr auto kType = "AMMO";

  // DATA, SSE (UESP "Skyrim Mod:Mod File Format/AMMO"; read from the lab's
  // 1.6.1170 masters with thuum lab/esm.py, docs/verbs/marksman.md): the
  // projectile 0x00, flags 0x04, damage 0x08, value 0x0C, weight 0x10
  struct Data
  {
    uint32_t projectile = 0;
    float damage = 0.f;
    float weight = 0.f;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const;
};

static_assert(sizeof(AMMO) == sizeof(RecordHeader));

}

#pragma pack(pop)
