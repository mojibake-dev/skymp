#include "libespm/CELL.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

CELL::Data CELL::GetData(CompressedFieldsCache& cache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "DATA", 4)) {
        // TODO: support size == 1, docs says it is possible in vanila skyrim
        result.flags = *reinterpret_cast<const uint16_t*>(data);
      }
    },
    cache);
  return result;
}

bool CELL::GetGrid(int32_t& outX, int32_t& outY,
                   CompressedFieldsCache& cache) const noexcept
{
  bool found = false;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "XCLC", 4) && size >= 8) {
        std::memcpy(&outX, data, 4);
        std::memcpy(&outY, data + 4, 4);
        found = true;
      }
    },
    cache);
  return found;
}

}
