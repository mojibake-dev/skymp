#include "libespm/HDPT.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

HDPT::Data HDPT::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!std::memcmp(type, "DATA", 4) && dataSize >= 1) {
        result.flags = static_cast<uint8_t>(data[0]);
      } else if (!std::memcmp(type, "PNAM", 4) && dataSize >= 4) {
        std::memcpy(&result.type, data, 4);
      } else if (!std::memcmp(type, "HNAM", 4) && dataSize >= 4) {
        uint32_t part = 0;
        std::memcpy(&part, data, 4);
        result.extraParts.push_back(part);
      } else if (!std::memcmp(type, "RNAM", 4) && dataSize >= 4) {
        std::memcpy(&result.validRaces, data, 4);
      }
    },
    compressedFieldsCache);
  return result;
}

}
