#pragma once
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

// thuum ADR-073 step 4's S6 (Eli's D-NT4 A): a topic info, a line of a
// dialogue topic, whose VMAD attaches its fragment script (Skyrim.esm's INFO
// 0x24528 attaches TIF__00024528, lab/esm.py, 2026-11-15). It sits in its
// topic's TOPIC_CHILDREN group, labelled with the DIAL's id
// (GroupHeader::GetParentDIAL), and its quest is that topic's (DIAL::Data)
class INFO final : public RecordHeader
{
public:
  static constexpr auto kType = "INFO";
};

static_assert(sizeof(INFO) == sizeof(RecordHeader));

}

#pragma pack(pop)
