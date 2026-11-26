#pragma once
#include "CONT.h"
#include "ObjectBounds.h"
#include "RecordHeader.h"
#include <set>

#pragma pack(push, 1)

namespace espm {

class NPC_ final : public RecordHeader
{
public:
  static constexpr auto kType = "NPC_";

  struct Faction
  {
    uint32_t formId = 0;
    int8_t rank = 0;
  };

  enum TemplateFlags : uint16_t
  {
    // (Destructible Object; Traits tab, including race, gender, height,
    // weight, voice type, death item; Sounds tab; Animation tab; Character Gen
    // tabs)
    UseTraits = 0x01,
    // (Stats tab, including level, autocalc, skills, health/magicka/stamina,
    // speed, bleedout, class)
    UseStats = 0x02,
    // (both factions and assigned crime faction)
    UseFactions = 0x04,
    // (both spells and perks)
    UseSpelllist = 0x08,
    // (AI Data tab, including aggression/confidence/morality, combat style and
    // gift filter)
    UseAIData = 0x10,
    // (only the basic Packages listed on the AI Packages tab; rest of tab
    // controlled by Def Pack List)
    UseAIPackages = 0x20,
    // Unused?
    Unused = 0x40,
    // (including name and short name, and flags for Essential, Protected,
    // Respawn, Summonable, Simple Actor, and Doesn't affect stealth meter)
    UseBaseData = 0x80,
    // (Inventory tab, including all outfits and geared-up item -- but not
    // death item)
    UseInventory = 0x100,
    // Scripts
    UseScript = 0x200,
    // (the dropdown-selected package lists on the AI Packages tab)
    UseDefPackList = 0x400,
    // (Attack Data tab, including override from behavior graph race, events,
    // and data)
    UseAttackData = 0x800,
    // Keywords
    UseKeywords = 0x1000
  };

  struct Data
  {
    uint32_t defaultOutfitId = 0;
    uint32_t sleepOutfitId = 0;

    std::set<uint32_t> spells = {};

    std::vector<CONT::ContainerObject> objects;
    std::vector<Faction> factions;

    bool isEssential = false;
    bool isUnique = false;
    bool isProtected = false;

    uint32_t race = 0;
    int16_t healthOffset = 0;
    int16_t magickaOffset = 0;
    int16_t staminaOffset = 0;
    ObjectBounds objectBounds = {};
    uint32_t baseTemplate = 0;
    uint16_t templateDataFlags = 0;
    uint32_t deathItem = 0;
    // ACBS (CommonLibSSE-NG include/RE/T/TESActorBaseData.h:16-78, the
    // engine's ACTOR_BASE_DATA): the auto-calc and the PC level mult flags
    // (1 << 4 and 1 << 7), the level (the mult times 1000 with the PC level
    // mult) and its clamps; CNAM, the class (a file-local form id)
    bool autoCalcStats = false;
    bool pcLevelMult = false;
    uint16_t level = 0;
    uint16_t calcLevelMin = 0;
    uint16_t calcLevelMax = 0;
    uint32_t classId = 0;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(NPC_) == sizeof(RecordHeader));

}

#pragma pack(pop)
