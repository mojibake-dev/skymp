#pragma once
#include <cstdint>
#include <optional>

#pragma pack(push, 1)

namespace espm {

struct ObjectBounds
{
  int16_t pos1[3] = { 0, 0, 0 };
  int16_t pos2[3] = { 0, 0, 0 };
};

static_assert(sizeof(ObjectBounds) == 12);

}

#pragma pack(pop)

namespace espm {

class RecordHeader;
class CompressedFieldsCache;

// The OBND field of any base record that carries one (every placeable type
// does): the object's box in its own units, before the reference's scale.
std::optional<ObjectBounds> GetObjectBounds(
  const RecordHeader* rec, CompressedFieldsCache& compressedFieldsCache);

// The farthest any point of the box is from the object's origin, before the
// reference's scale.
float BoundsRadius(const ObjectBounds& bounds);

}
