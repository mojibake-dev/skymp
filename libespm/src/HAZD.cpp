#include "libespm/HAZD.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

HAZD::Data HAZD::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "DATA", 4) && size >= 0x1C) {
        std::memcpy(&result.lifetime, data + 0x08, sizeof(result.lifetime));
        std::memcpy(&result.spell, data + 0x18, sizeof(result.spell));
      }
    },
    compressedFieldsCache);
  return result;
}

}
