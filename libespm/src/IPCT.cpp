#include "libespm/IPCT.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

IPCT::Data IPCT::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "NAM2", 4) && size >= 4) {
        std::memcpy(&result.hazard, data, sizeof(result.hazard));
      }
    },
    compressedFieldsCache);
  return result;
}

}
