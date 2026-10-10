#include "libespm/MGEF.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>
#include <type_traits>

namespace espm {

MGEF::Data MGEF::GetData(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  Data result;
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t size, const char* data) {
      if (!std::memcmp(type, "DATA", 4)) {
        result.data.flags = *reinterpret_cast<const Flags*>(data);
        result.data.effectType = EffectType{
          *reinterpret_cast<const std::underlying_type_t<EffectType>*>(data +
                                                                       0x40)
        };
        result.data.primaryAV = ActorValue(
          *reinterpret_cast<const std::underlying_type_t<ActorValue>*>(data +
                                                                       0x44));
        if (size >= 0x4C) {
          std::memcpy(&result.data.projectile, data + 0x48,
                      sizeof(result.data.projectile));
        }
        if (size >= 0x40) {
          std::memcpy(&result.data.secondAVWeight, data + 0x3C,
                      sizeof(result.data.secondAVWeight));
        }
        if (size >= 0x24) {
          std::memcpy(&result.data.hitShader, data + 0x20,
                      sizeof(result.data.hitShader));
        }
        if (size >= 0x5C) {
          std::underlying_type_t<ActorValue> second = 0;
          std::memcpy(&second, data + 0x58, sizeof(second));
          result.data.secondaryAV = ActorValue(second);
        }
        if (size >= 0x64) {
          std::memcpy(&result.data.hitEffectArt, data + 0x60,
                      sizeof(result.data.hitEffectArt));
        }
      }
    },
    compressedFieldsCache);

  return result;
}

}
