#include "libespm/SMQN.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

SMQN::Data SMQN::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!std::memcmp(type, "NNAM", 4) && dataSize >= sizeof(uint32_t)) {
        uint32_t formId = 0;
        std::memcpy(&formId, data, sizeof(formId));
        result.quests.push_back(formId);
      }
    },
    compressedFieldsCache);
  return result;
}

}
