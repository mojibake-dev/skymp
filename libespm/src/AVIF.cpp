#include "libespm/AVIF.h"
#include "libespm/CompressedFieldsCache.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>

namespace espm {

AVIF::Data AVIF::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data res;
  bool inTree = false;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "EDID", 4)) {
        res.editorId = data;
      } else if (!std::memcmp(type, "FULL", 4)) {
        res.fullName = data;
      } else if (!std::memcmp(type, "ANAM", 4)) {
        res.abbreviation = data;
      } else if (!std::memcmp(type, "AVSK", 4) && size >= 16) {
        res.hasSkill = true;
        std::memcpy(&res.skill.useMult, data, 4);
        std::memcpy(&res.skill.useOffset, data + 4, 4);
        std::memcpy(&res.skill.improveMult, data + 8, 4);
        std::memcpy(&res.skill.improveOffset, data + 12, 4);
      } else if (!std::memcmp(type, "PNAM", 4) && size >= 4) {
        inTree = true;
        Node node;
        std::memcpy(&node.perk, data, 4);
        res.nodes.push_back(node);
      } else if (!std::memcmp(type, "CNAM", 4) && size >= 4) {
        uint32_t value = 0;
        std::memcpy(&value, data, 4);
        if (inTree && !res.nodes.empty()) {
          res.nodes.back().connections.push_back(value);
        } else {
          res.category = value;
        }
      } else if (inTree && !res.nodes.empty() && size >= 4) {
        Node& node = res.nodes.back();
        if (!std::memcmp(type, "FNAM", 4)) {
          std::memcpy(&node.flag, data, 4);
        } else if (!std::memcmp(type, "XNAM", 4)) {
          std::memcpy(&node.x, data, 4);
        } else if (!std::memcmp(type, "YNAM", 4)) {
          std::memcpy(&node.y, data, 4);
        } else if (!std::memcmp(type, "HNAM", 4)) {
          std::memcpy(&node.h, data, 4);
        } else if (!std::memcmp(type, "VNAM", 4)) {
          std::memcpy(&node.v, data, 4);
        } else if (!std::memcmp(type, "SNAM", 4)) {
          std::memcpy(&node.skill, data, 4);
        } else if (!std::memcmp(type, "INAM", 4)) {
          std::memcpy(&node.index, data, 4);
        }
      }
    },
    compressedFieldsCache);
  return res;
}

}
