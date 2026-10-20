#pragma once
#include "ActorValue.h"
#include "CTDA.h"
#include "RecordHeader.h"
#include <vector>

#pragma pack(push, 1)

namespace espm {

class MGEF final : public RecordHeader
{
public:
  static constexpr auto kType = "MGEF";

  enum class EffectType : uint32_t
  {
    ValueMod = 0,
    Script,
    Dispel,
    CureDisease,
    Absorb,
    Dual,
    Calm,
    Demoralize,
    Frenzy,
    Disarm,
    CommandSummoned,
    Invisibility,
    Light,
    Lock = 15,
    Open,
    BoundWeapon,
    SummonCreature,
    DetectLife,
    Telekinesis,
    Paralysis,
    Reanimate,
    SoulTrap,
    TurnUndead,
    Guide,
    WerewolfFeed,
    CureParalysis,
    CureAddiction,
    CurePoison,
    Concussion,
    ValueAndParts,
    AccumulateMagnitude,
    Stagger,
    PeakValueMod,
    Cloak,
    Werewolf,
    SlowTime,
    Rally,
    EnchanceWeapon,
    SpawnHazard,
    Etherealize,
    Banish,
    SpawnScriptedRef,
    Disguise,
    GrabActor,
    VampireLord
  };
  static_assert(static_cast<std::underlying_type_t<EffectType>>(
                  EffectType::VampireLord) == 46);

  enum class Flags : uint32_t
  {
    Hostile = 0x00000001,
    Recover = 0x00000002,
    Detrimental = 0x00000004,
    SnapToNavmesh = 0x00000008,
    NoHitEvent = 0x00000010,
    DispelEffects = 0x00000100,
    NoDuration = 0x00000200,
    NoMagnitude = 0x00000400,
    NoArea = 0x00000800,
    FXPersist = 0x00001000,
    GoryVisual = 0x00004000,
    HideInUI = 0x00008000,
    NoRecast = 0x00020000,
    PowerAffectsMagnitude = 0x00200000,
    PowerAffectsDuration = 0x00400000,
    Painless = 0x04000000,
    NoHitEffect = 0x08000000,
    NoDeathDispel = 0x10000000
  };

  struct DATA
  {
    // primary actor value
    Flags flags;
    ActorValue primaryAV = espm::ActorValue::None;
    EffectType effectType;
    // the projectile (PROJ) the effect launches, 0 for none: DATA 0x48
    // (UESP "Skyrim Mod:Mod File Format/MGEF", after the effect type at 0x40
    // and the primary actor value at 0x44; thuum docs/verbs/spell-cast.md)
    uint32_t projectile = 0;
    // thuum docs/verbs/magic-effects.md, the same layout, checked against
    // the masters with thuum lab/esm.py: a dual value modifier's second
    // actor value (0x58, None for none) and its share of the magnitude
    // (0x3C); the hit shader (EFSH, 0x20) and the hit effect art (ARTO,
    // 0x60) an effect shows on its target, 0 for none
    ActorValue secondaryAV = espm::ActorValue::None;
    float secondAVWeight = 0.f;
    uint32_t hitShader = 0;
    uint32_t hitEffectArt = 0;
    // thuum docs/verbs/second-hand.md: the associated item (DATA 0x08, the
    // same UESP layout), whose meaning the archetype gives: a Cloak effect's
    // is the spell it applies to those near its bearer; 0 for none
    uint32_t associatedItem = 0;
    // thuum docs/verbs/spell-cast.md (train-27, the target by first hit):
    // how the effect reaches its target, DATA 0x54 (the same UESP layout:
    // casting type at 0x50, delivery at 0x54; MagicSystem::Delivery,
    // CommonLibSSE-NG include/RE/M/MagicSystem.h:39-45: 0 Self, 1 Touch,
    // 2 Aimed, 3 TargetActor, 4 TargetLocation), and the casting type at
    // 0x50 (MagicSystem::CastingType: 0 constant, 1 fire-and-forget, 2
    // concentration, 3 scroll); both 0 when the record is too short
    uint32_t castingType = 0;
    uint32_t delivery = 0;
    // thuum docs/verbs/magic-effects.md (train-30, resistance): the actor
    // value that resists the effect, DATA 0x10 (UESP "Skyrim Mod:Mod File
    // Format/MGEF": "ResistanceAV, Resistance skill index (generally 39-45)
    // or -1"); -1 for none, as when the record is too short
    int32_t resistValue = -1;

    [[nodiscard]] inline bool IsFlagSet(Flags flag) const
    {
      return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) ==
        static_cast<uint32_t>(flag);
    }
  };

  // the condition function HasPerk (CommonLibSSE-NG
  // include/RE/T/TESCondition.h:566, FUNCTION_DATA::FunctionID::kHasPerk)
  static constexpr uint16_t kHasPerkFunction = 448;

  struct Data
  {
    DATA data;
    // thuum docs/verbs/magic-effects.md: whether the effect record's own
    // conditions (its CTDA subrecords) ask HasPerk, the way vanilla gates a
    // perk's rider on the effect instead of the spell (the Impact perk's
    // stagger, Disintegrate's 200 Health)
    bool callsHasPerk = false;
    // thuum docs/DECISIONS.md ADR-039: the effect record's own conditions,
    // every CTDA in file order (the server evaluates them before applying
    // the effect to another player; an unknown function refuses it)
    std::vector<CTDA> conditions;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(MGEF) == sizeof(RecordHeader));

}

#pragma pack(pop)
