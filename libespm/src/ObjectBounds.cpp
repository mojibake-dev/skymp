#include "libespm/ObjectBounds.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace espm {

std::optional<ObjectBounds> GetObjectBounds(
  const RecordHeader* rec, CompressedFieldsCache& compressedFieldsCache)
{
  std::optional<ObjectBounds> result;
  if (!rec) {
    return result;
  }
  RecordHeaderAccess::IterateFields(
    rec,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!result && !std::memcmp(type, "OBND", 4) &&
          dataSize >= sizeof(ObjectBounds)) {
        ObjectBounds bounds;
        std::memcpy(&bounds, data, sizeof(bounds));
        result = bounds;
      }
    },
    compressedFieldsCache);
  return result;
}

float BoundsRadius(const ObjectBounds& bounds)
{
  // The corners are relative to the object's origin, which need not be the
  // box's center (a tree's sits at its base): per axis, the farther face.
  float sum = 0.f;
  for (int i = 0; i < 3; ++i) {
    const float reach =
      std::max(std::fabs(static_cast<float>(bounds.pos1[i])),
               std::fabs(static_cast<float>(bounds.pos2[i])));
    sum += reach * reach;
  }
  return std::sqrt(sum);
}
}
