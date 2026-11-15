#include "libespm/DIAL.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

DIAL::Data DIAL::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!std::memcmp(type, "QNAM", 4) && dataSize >= sizeof(uint32_t)) {
        std::memcpy(&result.quest, data, sizeof(result.quest));
      }
    },
    compressedFieldsCache);
  return result;
}

}
