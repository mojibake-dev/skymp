#pragma once
#include "RecordHeader.h"
#include <cstdint>
#include <vector>

#pragma pack(push, 1)

namespace espm {

// thuum ADR-073 step 5a (D-L2 B): a scene. Its quest is its own PNAM
// (CommonLibSSE-NG include/RE/B/BGSScene.h:94, `parentQuest`) and its actors
// the quest's aliases its ALIDs name (BGSScene.h:90, `actors`), both outside
// its actions: an action opens with an ANAM carrying its type and closes
// with an empty ANAM, and the ALID and PNAMs between are the action's own
// (its actor, BGSSceneAction.h:54, and a package action's packages).
// Skyrim.esm as read on sky-srv 2026-11-14: MS13LucanCamillaSceneView
// 0x3839b names aliases 0 and 2 ahead of its actions and quest 0x38386 in its
// last PNAM, after its actions' packages 0x383c0, 0x383bf and 0x63b2a;
// VC01VampirismCureRitual 0x5dba7 names alias 0 and VC01 0x5c625
class SCEN final : public RecordHeader
{
public:
  static constexpr auto kType = "SCEN";

  struct Data
  {
    // the scene's quest (PNAM outside an action), this file's id; 0 for none
    uint32_t quest = 0;
    // the scene's actors, its quest's alias ids, in the record's order
    std::vector<uint32_t> actorAliases;
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};

static_assert(sizeof(SCEN) == sizeof(RecordHeader));

}

#pragma pack(pop)
