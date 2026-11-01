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
  uint8_t tab = 0;
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
      } else if (!std::memcmp(type, "PRKE", 4) && size >= 3) {
        // a section (PRKE: type, rank, priority) ends the perk's own
        // conditions and opens an entry; what follows until PRKF is its
        inSections = true;
        tab = 0;
        Data::Entry entry;
        entry.type = static_cast<uint8_t>(data[0]);
        entry.rank = static_cast<uint8_t>(data[1]);
        entry.priority = static_cast<uint8_t>(data[2]);
        res.entries.push_back(entry);
      } else if (!std::memcmp(type, "DATA", 4) && size == 3) {
        inSections = true;
        if (!res.entries.empty()) {
          res.entries.back().entryPoint = static_cast<uint8_t>(data[0]);
          res.entries.back().function = static_cast<uint8_t>(data[1]);
          res.entries.back().tabCount = static_cast<uint8_t>(data[2]);
        }
      } else if (!std::memcmp(type, "PRKC", 4) && size >= 1) {
        tab = static_cast<uint8_t>(data[0]);
      } else if (!std::memcmp(type, "EPFT", 4) && size >= 1) {
        if (!res.entries.empty()) {
          res.entries.back().dataType = static_cast<uint8_t>(data[0]);
        }
      } else if (!std::memcmp(type, "EPFD", 4) && size >= 4) {
        if (!res.entries.empty()) {
          std::memcpy(&res.entries.back().value, data, 4);
        }
      } else if (!std::memcmp(type, "CTDA", 4) && size >= sizeof(CTDA)) {
        const auto& ctda = *reinterpret_cast<const CTDA*>(data);
        if (!inSections) {
          res.conditions.push_back(ctda);
        } else if (!res.entries.empty()) {
          res.entries.back().conditions.emplace_back(tab, ctda);
        }
      }
    },
    compressedFieldsCache);
  return res;
}

}
