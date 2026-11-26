#include "TestUtils.hpp"
#include "libespm/GroupUtils.h"
#include "libespm/Loader.h"
#include <catch2/catch_all.hpp>

#include "GetBaseActorValues.h"

extern espm::Loader l;
PartOne& GetPartOne();

TEST_CASE("GetBaseActorValues works correctly", "[GetBaseActorValues]")
{
  // Ri'saad is a Khajiit roving merchant from caravan.
  const uint32_t kRisaadFormId = 0x0001B1DB;

  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(kRisaadFormId, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kRisaadFormId);
  auto& ac = p.worldState.GetFormAt<MpActor>(kRisaadFormId);

  uint32_t baseId = ac.GetBaseId();
  auto appearance = ac.GetAppearance();
  uint32_t raceId = appearance ? appearance->raceId : 0;
  BaseActorValues baseValues =
    GetBaseActorValues(&p.worldState, baseId, raceId, {});

  REQUIRE(baseValues.health == 100.f);
  REQUIRE(baseValues.stamina == 100.f);
  REQUIRE(baseValues.magicka == 100.f);
  REQUIRE(baseValues.healRate == 0.7f);
  REQUIRE(baseValues.staminaRate == 5.f);
  REQUIRE(baseValues.magickaRate == 3.f);
}

// An auto-calc NPC's base health, magicka and stamina as the engine computes
// them (the TESNPC autocalc, id 24765, and its share of the level's points,
// 24899): its race's start plus its ACBS offset, the health's level bonus
// (level - 1) * fNPCHealthLevelBonus, and (level - 1) * iAVDhmsLevelUp points
// shared out by its class's attribute weights. EncWolf 0x23abe (Skyrim.esm:
// ACBS auto-calc, level 2, no offsets; WolfRace 0x1320a 12, 0, 200; its class
// EncClassAnimalPredator 0x131e6 weighs health 3, magicka 0, stamina 3) is
// the shared wolf both seats' engines hold at 22 health. An NPC that is not
// auto-calc keeps its race's start plus its offset.
TEST_CASE("An auto-calc NPC's base values are the engine's: its level's "
          "bonus and its class's share of the level's points on its race's "
          "start, a shared wolf at 22 health",
          "[GetBaseActorValues][espm]")
{
  PartOne& p = GetPartOne();
  const BaseActorValues wolf =
    GetBaseActorValues(&p.worldState, 0x23abe, 0, {});
  REQUIRE(wolf.health == 22.f);
  REQUIRE(wolf.stamina == 205.f);
  REQUIRE(wolf.magicka == 0.f);
  // Ri'saad's own base 0x1b1db is not auto-calc: Khajiit's 50 and his 100
  const BaseActorValues risaad =
    GetBaseActorValues(&p.worldState, 0x1b1db, 0, {});
  REQUIRE(risaad.health == 150.f);
}
