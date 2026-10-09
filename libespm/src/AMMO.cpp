#include "libespm/AMMO.h"
#include "libespm/RecordHeaderAccess.h"

namespace espm {

AMMO::Data AMMO::GetData(CompressedFieldsCache& cache) const
{
  Data res;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "DATA", 4)) {
        if (size >= 0x0C) {
          res.projectile = *reinterpret_cast<const uint32_t*>(data);
          res.damage = *reinterpret_cast<const float*>(data + 0x08);
        }
        // NOTE: 0x10 offset is for SSE version only
        if (size >= 0x14) {
          res.weight = *reinterpret_cast<const float*>(data + 0x10);
        }
      }
    },
    cache);
  return res;
}

}
