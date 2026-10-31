#include "libespm/REGN.h"
#include "libespm/CompressedFieldsCache.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

REGN::Data REGN::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data res;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "EDID", 4)) {
        res.editorId = data;
      } else if (!std::memcmp(type, "WNAM", 4) && size >= 4) {
        std::memcpy(&res.worldspace, data, 4);
      } else if (!std::memcmp(type, "RDWT", 4)) {
        for (uint32_t offset = 0; offset + 12 <= size; offset += 12) {
          WeatherEntry entry;
          std::memcpy(&entry.weather, data + offset, 4);
          std::memcpy(&entry.chance, data + offset + 4, 4);
          std::memcpy(&entry.global, data + offset + 8, 4);
          res.weathers.push_back(entry);
        }
      }
    },
    compressedFieldsCache);
  return res;
}

}
