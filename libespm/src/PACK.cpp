#include "libespm/PACK.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

PACK::Data PACK::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  bool found = false;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!found && !std::memcmp(type, "QNAM", 4) &&
          dataSize >= sizeof(uint32_t)) {
        std::memcpy(&result.ownerQuest, data, sizeof(uint32_t));
        found = true;
      }
    },
    compressedFieldsCache);
  return result;
}

}
