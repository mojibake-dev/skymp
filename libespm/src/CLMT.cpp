#include "libespm/CLMT.h"
#include "libespm/CompressedFieldsCache.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

CLMT::Data CLMT::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data res;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "EDID", 4)) {
        res.editorId = data;
      } else if (!std::memcmp(type, "TNAM", 4) && size >= 5) {
        res.hasTiming = true;
        res.volatility = static_cast<uint8_t>(data[4]);
      }
    },
    compressedFieldsCache);
  return res;
}

}
