#include "libespm/KEYM.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

KEYM::Data KEYM::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t, const char* data) {
      if (!std::memcmp(type, "EDID", 4)) {
        result.editorId = data;
      } else if (!std::memcmp(type, "FULL", 4)) {
        result.fullName = data;
      }
    },
    compressedFieldsCache);
  return result;
}

}
