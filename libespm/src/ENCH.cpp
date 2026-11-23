#include "libespm/ENCH.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

ENCH::Data ENCH::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  result.effects = Effects(this).GetData(compressedFieldsCache).effects;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "ENIT", 4) && size >= 0x18) {
        std::memcpy(&result.castType, data + 0x08, sizeof(result.castType));
        std::memcpy(&result.delivery, data + 0x10, sizeof(result.delivery));
        std::memcpy(&result.enchantType, data + 0x14,
                    sizeof(result.enchantType));
      }
    },
    compressedFieldsCache);
  return result;
}

}
