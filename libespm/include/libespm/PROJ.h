#pragma once
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

class PROJ final : public RecordHeader
{
public:
  static constexpr auto kType = "PROJ";

  // DATA (UESP "Skyrim Mod:Mod File Format/PROJ"; thuum
  // docs/verbs/spell-cast.md, read from the lab's 1.6.1170 masters with
  // thuum lab/esm.py): flags 0x00 (uint16), type 0x02 (uint16: 0x01
  // missile, 0x02 lobber, 0x04 beam, 0x08 flame, 0x10 cone, 0x20 barrier,
  // 0x40 arrow), gravity 0x04, speed 0x08, range 0x0C
  struct Data
  {
    uint16_t type = 0;
    float speed = 0.f;
    float range = 0.f;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const;
};

static_assert(sizeof(PROJ) == sizeof(RecordHeader));

}

#pragma pack(pop)
