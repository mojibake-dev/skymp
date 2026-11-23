#pragma once
#include "Effects.h"
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

class ENCH final : public RecordHeader
{
public:
  static constexpr auto kType = "ENCH";

  // ENIT (UESP "Skyrim Mod:Mod File Format/ENCH", read from the lab's
  // 1.6.1170 masters with thuum lab/esm.py: StaffEnchFlames 0x4DEDA reads
  // cost 14, flags 0, cast type 2, amount 14, delivery 2, enchant type 0x0C):
  // the cast type 0x08 (0 constant, 1 fire-and-forget, 2 concentration, as
  // SPEL's), the delivery 0x10 and the enchant type 0x14 (0x06 a weapon's
  // or an armor's enchantment, 0x0C a staff's); all 0 when the record has
  // no ENIT
  static constexpr uint32_t kCastConcentration = 2;
  static constexpr uint32_t kStaffEnchantment = 0x0C;

  struct Data
  {
    std::vector<Effects::Effect> effects;
    uint32_t castType = 0;
    uint32_t delivery = 0;
    uint32_t enchantType = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(ENCH) == sizeof(RecordHeader));

}

#pragma pack(pop)
