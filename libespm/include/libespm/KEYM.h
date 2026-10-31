#pragma once
#include "RecordHeader.h"

#pragma pack(push, 1)

namespace espm {

// A key (thuum docs/verbs/locks.md): a MISC-like item whose form id a lock's
// XLOC names; its editor id and name. Items of this type are picked up and
// held as any other (utils::IsItem).
class KEYM final : public RecordHeader
{
public:
  static constexpr auto kType = "KEYM";

  struct Data
  {
    const char* editorId = "";
    const char* fullName = "";
  };

  Data GetData(CompressedFieldsCache& compressedFieldsCache) const noexcept;
};
static_assert(sizeof(KEYM) == sizeof(RecordHeader));

}

#pragma pack(pop)
