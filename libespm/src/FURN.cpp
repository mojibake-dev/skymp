#include "libespm/FURN.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

FURN::Data FURN::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "MNAM", 4) && size >= sizeof(uint32_t)) {
        std::memcpy(&result.activeMarkers, data, sizeof(uint32_t));
      }
    },
    compressedFieldsCache);
  return result;
}

}
