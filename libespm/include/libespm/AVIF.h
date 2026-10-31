#pragma once
#include "RecordHeader.h"
#include <cstdint>
#include <vector>

#pragma pack(push, 1)

namespace espm {

// An actor value's information record (thuum docs/verbs/leveling.md and the
// perk verb to come): the skill's experience constants (AVSK: skill use mult,
// skill use offset, skill improve mult, skill improve offset; CommonLibSSE-NG
// ActorValueInfo::Skill) and, for the eighteen skills, the perk tree as the
// Creation Kit lays it out: one node per PNAM (the perk, 0 for the tree's
// root), its FNAM flag, XNAM and YNAM grid cell, HNAM and VNAM position, SNAM
// (the skill's own AVIF), every CNAM after the PNAM a connection to another
// node's INAM, and INAM the node's own index (UESP, "Skyrim Mod:Mod File
// Format/AVIF"). The CNAM before the first PNAM is the skill's category. The
// form ids are the file's own; a caller makes them global through its lookup
// result.
class AVIF final : public RecordHeader
{
public:
  static constexpr auto kType = "AVIF";

  struct Skill
  {
    float useMult = 0.f;
    float useOffset = 0.f;
    float improveMult = 0.f;
    float improveOffset = 0.f;
  };

  struct Node
  {
    uint32_t perk = 0;                 // PNAM; 0 for the root
    uint32_t flag = 0;                 // FNAM
    uint32_t x = 0;                    // XNAM
    uint32_t y = 0;                    // YNAM
    float h = 0.f;                     // HNAM
    float v = 0.f;                     // VNAM
    uint32_t skill = 0;                // SNAM, the skill's AVIF
    uint32_t index = 0;                // INAM
    std::vector<uint32_t> connections; // CNAM: the INAM of each node reached
  };

  struct Data
  {
    const char* editorId = "";
    const char* fullName = "";
    const char* abbreviation = "";
    uint32_t category = 0;
    bool hasSkill = false;
    Skill skill;
    std::vector<Node> nodes;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(AVIF) == sizeof(RecordHeader));

}

#pragma pack(pop)
