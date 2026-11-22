#include "libespm/WEAP.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

WEAP::Data WEAP::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!std::memcmp(type, "DATA", 4)) {
        result.weapData = reinterpret_cast<const WeapData*>(data);
      } else if (!std::memcmp(type, "EITM", 4) && dataSize >= 4) {
        std::memcpy(&result.enchantmentFormId, data, sizeof(uint32_t));
      } else if (!std::memcmp(type, "DNAM", 4)) {
        result.weapDNAM = reinterpret_cast<const DNAM*>(data);
        constexpr uint32_t kStaggerOffset = 0x60;
        if (dataSize >= kStaggerOffset + sizeof(float)) {
          std::memcpy(&result.stagger, data + kStaggerOffset, sizeof(float));
        }
      } else if (!std::memcmp(type, "ETYP", 4) &&
                 dataSize >= sizeof(uint32_t)) {
        std::memcpy(&result.equipSlotId, data, sizeof(uint32_t));
      }
    },
    compressedFieldsCache);
  return result;
}

}
