#pragma once
#include <array>
#include <cstdint>

namespace espm {

// One file's form ids to the combined numbering, or the combined numbering
// back to the file's own. The combined numbering is the engine's (thuum
// docs/verbs/light-plugins.md; CommonLibSSE-NG src/RE/T/TESFile.cpp:33-42,
// src/RE/T/TESDataHandler.cpp:40-87): a full plugin's forms sit at its index
// among full plugins << 24 with a 24-bit local id, a light plugin's at
// 0xFE000000 | its index among light plugins << 12 with a 12-bit local id.
// Inside a file an id's top byte indexes the file's master list, the file
// itself last. An id maps by its file: its top byte, or for a light id (top
// byte 0xFE) its 12-bit light index; an id whose file has no counterpart on
// the other side maps to 0xFF000000 | its low 24 bits, which callers skip.
class IdMapping
{
public:
  struct File
  {
    bool light = false;
    uint16_t index = 0; // a top byte, or a light index (0 to 0xFFF)
  };

  // every file unmapped
  IdMapping() noexcept;

  void Set(File from, File to) noexcept;

  uint32_t Map(uint32_t id) const noexcept;

private:
  static constexpr uint16_t kNone = 0xFFFF;
  struct Target
  {
    uint16_t index = kNone;
    bool light = false;
  };
  std::array<Target, 256> full;
  std::array<Target, 4096> light;
};

}
