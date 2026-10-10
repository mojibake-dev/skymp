#include "libespm/PROJ.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

PROJ::Data PROJ::GetData(CompressedFieldsCache& cache) const
{
  Data res;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "DATA", 4) && size >= 0x10) {
        std::memcpy(&res.type, data + 0x02, sizeof(res.type));
        std::memcpy(&res.speed, data + 0x08, sizeof(res.speed));
        std::memcpy(&res.range, data + 0x0C, sizeof(res.range));
      }
    },
    cache);
  return res;
}

}
