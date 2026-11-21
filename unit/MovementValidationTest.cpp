#include <catch2/catch_all.hpp>

#include "MovementValidation.h"
#include "MsgType.h"
#include "NiPoint3.h"
#include "TestUtils.hpp"
#include <vector>

extern PartOne& GetPartOne();

TEST_CASE("Returns true and sends nothing for normal movement",
          "[MovementValidation]")
{
  PartOne& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);

  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);

  partOne.Messages().clear();
  bool res = MovementValidation::Validate(
    partOne, { 0, 0, 0 }, { 0, 0, 0 }, FormDesc::Tamriel(), { 1, 1, 1 },
    FormDesc::Tamriel(), 0, &actor, { "Skyrim.esm" });
  REQUIRE(res);
  REQUIRE(partOne.Messages().empty());
}

// thuum ADR-079 decision 7 (Eli, 2026-11-20, tracker #191): the player's own
// game moving it by a jump (4096 units or more) is recorded, never sent back,
// and placed for the other games; upstream sent the player back
TEST_CASE("A player's own jump of 4096 units or more is recorded, not sent "
          "back, and placed",
          "[MovementValidation]")
{
  PartOne& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);

  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);

  partOne.Messages().clear();
  float maxLegalMove = 4096.f;
  bool placed = false;
  bool res = MovementValidation::Validate(
    partOne, { 1, -1, 1 }, { 123, 111, 123 }, FormDesc::Tamriel(),
    NiPoint3{ 1, -1, 1 } + NiPoint3{ maxLegalMove + 1.f, 0, 0 },
    FormDesc::Tamriel(), 0, &actor, { "Skyrim.esm" }, &placed);
  REQUIRE(res);
  REQUIRE(placed);
  REQUIRE(partOne.Messages().empty());
}

// thuum ADR-079 decision 7: the player's own game moving it into another cell
// (a jail, a Black Book's Apocrypha, a script's MoveTo) is recorded, never
// sent back; another game's actor moving cells without the cell-change rule
// is still dropped, with nothing sent
TEST_CASE("A player's own move into another cell is recorded, not sent back, "
          "and another game's actor's is dropped",
          "[MovementValidation]")
{
  PartOne& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);
  partOne.CreateActor(0xff000001, { 0, 0, 0 }, 0, 0x3c);

  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  auto& npc = partOne.worldState.GetFormAt<MpActor>(0xff000001);

  partOne.Messages().clear();
  bool placed = false;
  bool res = MovementValidation::Validate(
    partOne, { 1, -1, 1 }, { 123, 111, 123 }, FormDesc::Tamriel(),
    { 1, -1, 1 }, FormDesc::FromString("ffffff:Skyrim.esm"), 0, &actor,
    { "Skyrim.esm" }, &placed);
  REQUIRE(res);
  REQUIRE(placed);
  REQUIRE(partOne.Messages().empty());

  res = MovementValidation::Validate(partOne, { 1, -1, 1 }, { 123, 111, 123 },
                                     FormDesc::Tamriel(), { 1, -1, 1 },
                                     FormDesc::FromString("ffffff:Skyrim.esm"),
                                     0, &npc, { "Skyrim.esm" });
  REQUIRE(!res);
  REQUIRE(partOne.Messages().empty());
}

TEST_CASE("A player beyond its ground speed budget is recorded, not sent back",
          "[MovementValidation]")
{
  // thuum docs/verbs/movement-speed.md: two 1000 unit moves at once fit the
  // 2048 unit burst, a third does not; since ADR-079 decision 7 (tracker
  // #191) the third is the player's own game's move all the same, recorded
  // and placed with nothing sent back; another user's actor is not charged
  PartOne& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);
  partOne.CreateActor(0xff000001, { 0, 0, 0 }, 0, 0x3c);

  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  auto& npc = partOne.worldState.GetFormAt<MpActor>(0xff000001);

  const auto move = [&](MpActor& who, bool* placed = nullptr) {
    return MovementValidation::Validate(
      partOne, { 0, 0, 0 }, { 0, 0, 0 }, FormDesc::Tamriel(), { 1000, 0, 50 },
      FormDesc::Tamriel(), 0, &who, { "Skyrim.esm" }, placed);
  };

  partOne.Messages().clear();
  bool placed = false;
  REQUIRE(move(actor, &placed));
  REQUIRE(!placed);
  REQUIRE(move(actor, &placed));
  REQUIRE(!placed);
  REQUIRE(move(actor, &placed));
  REQUIRE(placed);
  REQUIRE(partOne.Messages().empty());

  partOne.Messages().clear();
  for (int i = 0; i < 5; ++i) {
    REQUIRE(move(npc));
  }
  REQUIRE(partOne.Messages().empty());
}
