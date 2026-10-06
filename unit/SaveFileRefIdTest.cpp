#include "savefile/SFStructure.h"
#include <catch2/catch_all.hpp>

// thuum: the login save Skyrim Platform writes names a form from any plugin
// but Skyrim.esm through the save's formIDArray (RefID type 0, the index from
// 1); a race or head part from a DLC or a mod came back as the template's
// otherwise (docs/MODS.md, rotfern).

TEST_CASE("CreateRefId indexes a form in the save's formIDArray, reusing an "
          "entry and moving the tables after a new one",
          "[SaveFile]")
{
  SaveFile_::SaveFile save;
  save.formIDArray = { 0x0002abcd, 0x0300aa00 };
  save.formIDArrayCount = 2;
  save.fileLocationTable.unknownTable3Offset = 1000;

  // already there: the second entry, nothing grows
  const auto existing = SaveFile_::RefID::CreateRefId(save, 0x0300aa00);
  REQUIRE(existing.byte0 == 0);
  REQUIRE(existing.byte1 == 0);
  REQUIRE(existing.byte2 == 2);
  REQUIRE(save.formIDArrayCount == 2);
  REQUIRE(save.fileLocationTable.unknownTable3Offset == 1000);

  // new: the third entry, the old ones intact, the tables after it four
  // bytes on
  const auto added = SaveFile_::RefID::CreateRefId(save, 0x0800aa00);
  REQUIRE(added.byte0 == 0);
  REQUIRE(added.byte1 == 0);
  REQUIRE(added.byte2 == 3);
  REQUIRE(save.formIDArray ==
          std::vector<uint32_t>{ 0x0002abcd, 0x0300aa00, 0x0800aa00 });
  REQUIRE(save.formIDArrayCount == 3);
  REQUIRE(save.fileLocationTable.unknownTable3Offset == 1004);

  // the same form again: the same entry
  REQUIRE(SaveFile_::RefID::CreateRefId(save, 0x0800aa00).byte2 == 3);
  REQUIRE(save.formIDArrayCount == 3);
}
