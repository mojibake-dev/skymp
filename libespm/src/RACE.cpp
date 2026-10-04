#include "libespm/RACE.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <cstring>

namespace espm {

RACE::Data RACE::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "DATA", 4)) {
        result.height[0] = *reinterpret_cast<const float*>(data + 16);
        result.height[1] = *reinterpret_cast<const float*>(data + 20);
        result.flags = *reinterpret_cast<const uint32_t*>(data + 32);
        result.startingHealth = *reinterpret_cast<const float*>(data + 36);
        result.startingMagicka = *reinterpret_cast<const float*>(data + 40);
        result.startingStamina = *reinterpret_cast<const float*>(data + 44);
        result.healRegen = *reinterpret_cast<const float*>(data + 84);
        result.magickaRegen = *reinterpret_cast<const float*>(data + 88);
        result.staminaRegen = *reinterpret_cast<const float*>(data + 92);
        result.unarmedDamage = *reinterpret_cast<const float*>(data + 96);
        result.unarmedReach = *reinterpret_cast<const float*>(data + 100);
      } else if (!std::memcmp(type, "SPLO", 4)) {
        result.spells.emplace(*reinterpret_cast<const uint32_t*>(data));
      } else if (!std::memcmp(type, "ATKD", 4) && size >= 24) {
        // attack angle and strike angle: bytes 16 and 20 of ATKD (UESP;
        // CommonLibSSE-NG BGSAttackData::AttackData's order). Forward attacks
        // only: Update.esm's mounted side attacks (attackStart_MC_*: attack
        // angle 90 or -90, strike 85) aim a rider's cone to its side, and
        // mounted combat is not ruled yet
        const float attackAngle = *reinterpret_cast<const float*>(data + 16);
        if (attackAngle == 0.f) {
          result.widestStrikeAngle =
            std::max(result.widestStrikeAngle,
                     *reinterpret_cast<const float*>(data + 20));
        }
      }
    },
    compressedFieldsCache);
  return result;
}

}
