#include "savefile/SFChangeFormNPC.h"
#include <catch2/catch_all.hpp>
#include <cstring>

// thuum: the login save's NPC face block, as the game reads it: after the
// head parts, a byte, then the face options as [u32 count][floats], then the
// presets as [u32 count][u32s]. The counts were written as size_t (8 bytes),
// and the game, reading 4, misread the rest of the record, the gender after
// it included (apocrypha, from a real save, 2026-10-07; docs/PLAN.md).

namespace {
uint32_t U32At(const std::vector<uint8_t>& b, size_t at)
{
  REQUIRE(b.size() >= at + 4);
  uint32_t v;
  std::memcpy(&v, b.data() + at, 4);
  return v;
}
}

TEST_CASE("The face block writes its option and preset counts as 4 bytes",
          "[SaveFile]")
{
  SaveFile_::ChangeFormNPC_ npc;
  SaveFile_::ChangeFormNPC_::Face face;
  face.headParts = { SaveFile_::RefID{}, SaveFile_::RefID{} };
  face.options.assign(19, 0.5f);
  face.presets = { 1, 0xFFFFFFFF, 22, 15 };
  npc.face = face;
  npc.gender = 1;

  const auto [flags, bytes] = npc.ToBinary();
  // CHANGE_NPC_FACE and CHANGE_NPC_GENDER, as SFChangeFormNPC.cpp names them
  REQUIRE((flags & 0x00000800) != 0);
  REQUIRE((flags & 0x01000000) != 0);

  // 1 (the block's leading byte), the three forms, 1 (vsval: two parts),
  // the two parts, 1 (the byte before the options)
  const size_t optionsCountAt = 1 + sizeof(face.hairColorForm) +
    sizeof(face.bodySkinColor) + sizeof(face.headTextureSet) + 1 +
    2 * sizeof(SaveFile_::RefID) + 1;
  REQUIRE(U32At(bytes, optionsCountAt) == 19);
  const size_t presetsCountAt = optionsCountAt + 4 + 19 * sizeof(float);
  REQUIRE(U32At(bytes, presetsCountAt) == 4);
  REQUIRE(U32At(bytes, presetsCountAt + 4) == 1);
  REQUIRE(U32At(bytes, presetsCountAt + 8) == 0xFFFFFFFF);
  // the gender right after the presets, nothing in between
  REQUIRE(bytes.size() == presetsCountAt + 4 + 4 * sizeof(uint32_t) + 1);
  REQUIRE(bytes.back() == 1);
}
