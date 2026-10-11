#pragma once
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

// game settings
class GMST final : public RecordHeader
{
public:
  static constexpr auto kType = "GMST";

  static constexpr uint32_t kFCombatDistance = 0x00055640;
  static constexpr uint32_t kFCombatBashReach = 0x00055641;
  static constexpr uint32_t kFMaxArmorRating = 0x00037DEB;
  static constexpr uint32_t kFArmorScalingFactor = 0x00021A72;
  // The base sneak attack multipliers by weapon type, as Skyrim.esm holds
  // them (thuum lab/esm.py find GMST CombatSneak, 2026-10-04; thuum
  // docs/verbs/sneak-damage.md)
  static constexpr uint32_t kFCombatSneakHandMult = 0x00050DA3;
  static constexpr uint32_t kFCombatSneak1HSwordMult = 0x00050DA2;
  static constexpr uint32_t kFCombatSneak1HMaceMult = 0x00050DA1;
  static constexpr uint32_t kFCombatSneak1HAxeMult = 0x00050DA0;
  static constexpr uint32_t kFCombatSneak1HDaggerMult = 0x00050D9F;
  static constexpr uint32_t kFCombatSneak2HSwordMult = 0x00069F47;
  static constexpr uint32_t kFCombatSneak2HAxeMult = 0x00069F48;
  // The block settings, as Skyrim.esm holds them (thuum lab/esm.py find
  // GMST Block and Shield, 2026-10-10; thuum docs/verbs/blocking.md)
  static constexpr uint32_t kFShieldBaseFactor = 0x000130FE;
  static constexpr uint32_t kFShieldScalingFactor = 0x0003189B;
  static constexpr uint32_t kFBlockWeaponBase = 0x00045C4F;
  static constexpr uint32_t kFBlockWeaponScaling = 0x00045C50;
  static constexpr uint32_t kFBlockSkillMult = 0x0001A16A;
  static constexpr uint32_t kFBlockMax = 0x0001A169;
  static constexpr uint32_t kFBlockPowerAttackMult = 0x0004E609;

  struct Data
  {
    float value = 0.f;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(GMST) == sizeof(RecordHeader));

}

#pragma pack(pop)
