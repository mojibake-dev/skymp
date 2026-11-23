#include "libespm/IPDS.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

IPDS::Data IPDS::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "PNAM", 4) && size >= 8) {
        uint32_t impact = 0;
        std::memcpy(&impact, data + 0x04, sizeof(impact));
        result.impacts.push_back(impact);
      }
    },
    compressedFieldsCache);
  return result;
}

}
