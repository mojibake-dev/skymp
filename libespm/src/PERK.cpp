#include "libespm/PERK.h"
#include "libespm/CompressedFieldsCache.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

PERK::Data PERK::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data res;
  bool inSections = false;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "EDID", 4)) {
        res.editorId = data;
      } else if (!std::memcmp(type, "DATA", 4) && size == 5) {
        res.isTrait = data[0] != 0;
        res.level = static_cast<uint8_t>(data[1]);
        res.numRanks = static_cast<uint8_t>(data[2]);
        res.isPlayable = data[3] != 0;
        res.isHidden = data[4] != 0;
      } else if (!std::memcmp(type, "NNAM", 4) && size >= 4) {
        std::memcpy(&res.nextPerk, data, 4);
      } else if (!std::memcmp(type, "PRKE", 4) ||
                 (!std::memcmp(type, "DATA", 4) && size == 3)) {
        // the first section (PRKE: type, rank, priority) ends the perk's own
        // conditions; what follows belongs to the entries
        inSections = true;
      } else if (!inSections && !std::memcmp(type, "CTDA", 4) &&
                 size >= sizeof(CTDA)) {
        res.conditions.push_back(*reinterpret_cast<const CTDA*>(data));
      }
    },
    compressedFieldsCache);
  return res;
}

}
