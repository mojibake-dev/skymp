#include "libespm/CLAS.h"
#include "libespm/CompressedFieldsCache.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

CLAS::Data CLAS::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data res;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "EDID", 4)) {
        res.editorId = data;
      } else if (!std::memcmp(type, "DATA", 4) && size >= 35) {
        res.hasWeights = true;
        res.healthWeight = static_cast<uint8_t>(data[32]);
        res.magickaWeight = static_cast<uint8_t>(data[33]);
        res.staminaWeight = static_cast<uint8_t>(data[34]);
      }
    },
    compressedFieldsCache);
  return res;
}

}
