#include "libespm/SHOU.h"
#include "libespm/CompressedFieldsCache.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

SHOU::Data SHOU::GetData(CompressedFieldsCache& cache) const noexcept
{
  Data res;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "SNAM", 4) && size >= 12 &&
          res.count < res.variations.size()) {
        auto& variation = res.variations[res.count++];
        std::memcpy(&variation.word, data, 4);
        std::memcpy(&variation.spell, data + 4, 4);
        std::memcpy(&variation.recovery, data + 8, 4);
      }
    },
    cache);
  return res;
}

}
