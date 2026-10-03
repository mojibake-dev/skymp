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

TEST_CASE("Returns false and sends teleport packet when moving too fast",
          "[MovementValidation]")
{
  PartOne& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);

  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);

  partOne.Messages().clear();
  float maxLegalMove = 4096.f;
  bool res = MovementValidation::Validate(
    partOne, { 1, -1, 1 }, { 123, 111, 123 }, FormDesc::Tamriel(),
    NiPoint3{ 1, -1, 1 } + NiPoint3{ maxLegalMove + 1.f, 0, 0 },
    FormDesc::Tamriel(), 0, &actor, { "Skyrim.esm" });
  REQUIRE(!res);
  REQUIRE(partOne.Messages().size() == 1);
  REQUIRE(partOne.Messages()[0].j ==
          nlohmann::json{ { "t", static_cast<int>(MsgType::Teleport2) },
                          { "pos", { 1, -1, 1 } },
                          { "rot", { 123, 111, 123 } },
                          { "worldOrCell", 0x3c } });
  REQUIRE(partOne.Messages()[0].userId == 0);
}

TEST_CASE(
  "Returns false and sends teleport packet when moving between locations",
  "[MovementValidation]")
{
  PartOne& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);

  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);

  partOne.Messages().clear();
  bool res = MovementValidation::Validate(
    partOne, { 1, -1, 1 }, { 123, 111, 123 }, FormDesc::Tamriel(),
    { 1, -1, 1 }, FormDesc::FromString("ffffff:Skyrim.esm"), 0, &actor,
    { "Skyrim.esm" });
  REQUIRE(!res);
  REQUIRE(partOne.Messages().size() == 1);
  REQUIRE(partOne.Messages()[0].j ==
          nlohmann::json{ { "t", static_cast<int>(MsgType::Teleport2) },
                          { "pos", { 1, -1, 1 } },
                          { "rot", { 123, 111, 123 } },
                          { "worldOrCell", 0x3c } });
  REQUIRE(partOne.Messages()[0].userId == 0);
}

TEST_CASE("A player beyond its ground speed budget is sent back",
          "[MovementValidation]")
{
  // thuum docs/verbs/movement-speed.md: two 1000 unit moves at once fit the
  // 2048 unit burst, a third does not; another user's actor is not charged
  PartOne& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);
  partOne.CreateActor(0xff000001, { 0, 0, 0 }, 0, 0x3c);

  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  auto& npc = partOne.worldState.GetFormAt<MpActor>(0xff000001);

  const auto move = [&](MpActor& who) {
    return MovementValidation::Validate(
      partOne, { 0, 0, 0 }, { 0, 0, 0 }, FormDesc::Tamriel(), { 1000, 0, 50 },
      FormDesc::Tamriel(), 0, &who, { "Skyrim.esm" });
  };

  partOne.Messages().clear();
  REQUIRE(move(actor));
  REQUIRE(move(actor));
  REQUIRE(partOne.Messages().empty());
  REQUIRE(!move(actor));
  REQUIRE(partOne.Messages().size() == 1);
  REQUIRE(partOne.Messages()[0].j["t"] ==
          static_cast<int>(MsgType::Teleport2));

  partOne.Messages().clear();
  for (int i = 0; i < 5; ++i) {
    REQUIRE(move(npc));
  }
  REQUIRE(partOne.Messages().empty());
}
