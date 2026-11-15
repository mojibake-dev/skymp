#include "libespm/SCEN.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

SCEN::Data SCEN::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  bool inAction = false;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!std::memcmp(type, "ANAM", 4)) {
        // an action's type opens it, an empty ANAM closes it
        inAction = dataSize > 0;
        return;
      }
      if (inAction || dataSize < sizeof(uint32_t)) {
        return;
      }
      uint32_t value = 0;
      std::memcpy(&value, data, sizeof(value));
      if (!std::memcmp(type, "PNAM", 4)) {
        result.quest = value;
      } else if (!std::memcmp(type, "ALID", 4)) {
        result.actorAliases.push_back(value);
      }
    },
    compressedFieldsCache);
  return result;
}

}
