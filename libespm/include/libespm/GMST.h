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
  static constexpr uint32_t kFBlockPowerAttackMult = 0x0004E609;
  static constexpr uint32_t kFArmorRatingBase = 0x0001A157;
  static constexpr uint32_t kFArmorRatingMax = 0x0001A158;
  // The stagger settings Skyrim.esm sets (thuum lab/esm.py find GMST
  // Stagger, 2026-10-10; Update, Dawnguard and Dragonborn set none; thuum
  // docs/verbs/stagger.md); the others keep the executable's defaults
  static constexpr uint32_t kFStaggerPlayerMassMult = 0x000C3CBC;
  static constexpr uint32_t kFStaggerMassOffsetBase = 0x000E6C4E;
  static constexpr uint32_t kFStaggerMin = 0x000D33A3;
  static constexpr uint32_t kFStaggerBlockBase = 0x00055A99;
  static constexpr uint32_t kFStaggerBlockMult = 0x0003189C;
  static constexpr uint32_t kFStaggerBlockAttackShieldBase = 0x0004E50B;
  static constexpr uint32_t kFStaggerBlockAttackShieldMult = 0x00055A98;
  static constexpr uint32_t kFStaggerBlockingMult = 0x00015BDD;
  static constexpr uint32_t kIStaggerAttackChance = 0x0010B0DE;

  struct Data
  {
    float value = 0.f;
    // the same four bytes for an integer setting (an "i" name)
    int32_t intValue = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(GMST) == sizeof(RecordHeader));

}

#pragma pack(pop)
