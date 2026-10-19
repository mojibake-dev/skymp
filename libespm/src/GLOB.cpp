#include "libespm/GLOB.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

GLOB::Data GLOB::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "FNAM", 4) && size >= 1) {
        result.type = data[0];
      } else if (!std::memcmp(type, "FLTV", 4) && size >= sizeof(float)) {
        std::memcpy(&result.value, data, sizeof(float));
      }
    },
    compressedFieldsCache);
  return result;
}

}
