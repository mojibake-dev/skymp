#pragma once
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

class PROJ final : public RecordHeader
{
public:
  static constexpr auto kType = "PROJ";

  // DATA (UESP "Skyrim Mod:Mod File Format/PROJ"; thuum
  // docs/verbs/spell-cast.md, read from the lab's 1.6.1170 masters with
  // thuum lab/esm.py): flags 0x00 (uint16), type 0x02 (uint16: 0x01
  // missile, 0x02 lobber, 0x04 beam, 0x08 flame, 0x10 cone, 0x20 barrier,
  // 0x40 arrow), gravity 0x04, speed 0x08, range 0x0C
  // thuum docs/verbs/spell-cast.md (train-27, a placed trigger's claim):
  // the same layout past the range (xEdit wbDefinitionsTES5.pas, wbRecord
  // PROJ; UESP agrees; read by apocrypha 2026-10-18 on the lab's masters):
  // flags 0x00 (0x0002 explosion, 0x0004 alternate trigger), the alternate
  // trigger's proximity 0x1C (the radius an actor trips it in) and timer
  // 0x20 (seconds until it bursts by itself), the explosion 0x24. A rune is
  // a lobber with the alternate trigger, a proximity above 0 and no timer
  // (vanilla RuneFrostProjectile: 150 and 0); a delayed burst has a timer
  // and no proximity (Lightning Strike's: 0 and 2.5)
  struct Data
  {
    uint16_t flags = 0;
    uint16_t type = 0;
    float speed = 0.f;
    float range = 0.f;
    float triggerProximity = 0.f;
    float triggerTimer = 0.f;
    uint32_t explosion = 0;

    static constexpr uint16_t kFlagExplosion = 0x0002;
    static constexpr uint16_t kFlagAltTrigger = 0x0004;

    // a placed trigger that waits for an actor: its hit comes long after
    // its cast
    [[nodiscard]] bool IsPlacedTrigger() const
    {
      return (flags & kFlagAltTrigger) != 0 && triggerProximity > 0.f &&
        triggerTimer <= 0.f;
    }
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const;
};

static_assert(sizeof(PROJ) == sizeof(RecordHeader));

}

#pragma pack(pop)
