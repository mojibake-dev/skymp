#include "libespm/IdMapping.h"

namespace espm {

IdMapping::IdMapping() noexcept = default;

void IdMapping::Set(File from, File to) noexcept
{
  Target target;
  target.index = to.light ? static_cast<uint16_t>(to.index & 0x0fff)
                          : static_cast<uint16_t>(to.index & 0x00ff);
  target.light = to.light;
  if (from.light) {
    light[from.index & 0x0fff] = target;
  } else {
    full[from.index & 0x00ff] = target;
  }
}

uint32_t IdMapping::Map(uint32_t id) const noexcept
{
  const uint32_t top = id >> 24;
  const Target& target =
    top == 0xfe ? light[(id >> 12) & 0x0fff] : full[top];
  const uint32_t local = top == 0xfe ? (id & 0x00000fff) : (id & 0x00ffffff);
  if (target.index == kNone) {
    return 0xff000000 | (id & 0x00ffffff);
  }
  if (target.light) {
    return 0xfe000000 | (static_cast<uint32_t>(target.index) << 12) |
      (local & 0x00000fff);
  }
  return (static_cast<uint32_t>(target.index) << 24) | local;
}

}
