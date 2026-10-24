#include "TestUtils.hpp"
#include "libespm/Loader.h"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum docs/verbs/npc-fate.md (the memo's D2, the middle ground): the level
// a fresh square's leveled actors roll at, by the npcRollLevel setting
// (PartOne::FateRollLevel). Three players with records: two in the forest a
// square apart, one far across Tamriel; a fourth with no record yet.

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kStrong = 0xff000d08;
constexpr uint32_t kWeak = 0xff000d09;
constexpr uint32_t kFar = 0xff000d0a;
constexpr uint32_t kFresh = 0xff000d0b;
const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };
const NiPoint3 kForestEdge{ 123000.f + 4096.f, -80000.f, 10705.f };
const NiPoint3 kFarAway{ 0.f, 0.f, 0.f };
const NiPoint3 kElsewhere{ -50000.f, 50000.f, 0.f };

MpActor& Player(PartOne& p, Networking::UserId user, uint32_t id,
                const NiPoint3& pos)
{
  DoConnect(p, user);
  p.CreateActor(id, pos, 0, kTamriel);
  p.SetUserActor(user, id);
  return p.worldState.GetFormAt<MpActor>(id);
}

MpActor& Player(PartOne& p, Networking::UserId user, uint32_t id,
                const NiPoint3& pos, uint16_t level)
{
  auto& actor = Player(p, user, id, pos);
  ActorValueRecord record;
  record.level = level;
  actor.SetActorValueRecord(record);
  return actor;
}

int16_t Square(float coordinate)
{
  return static_cast<int16_t>(coordinate / 4096);
}
}

TEST_CASE("The roll's level follows npcRollLevel: the loader's own, the "
          "party's mean or low end within the fate ring, or a fixed number",
          "[NpcFate][NpcRollLevel]")
{
  auto& p = GetPartOne();
  const auto savedRule = p.worldState.npcRollLevel;
  const auto savedFixed = p.worldState.npcRollLevelFixed;
  auto& strong = Player(p, 0, kStrong, kForest, 30);
  Player(p, 1, kWeak, kForestEdge, 10);
  auto& far = Player(p, 2, kFar, kFarAway, 50);
  auto& fresh = Player(p, 3, kFresh, kElsewhere);
  const auto sx = Square(kForest.x);
  const auto sy = Square(kForest.y);

  p.worldState.npcRollLevel = "loader";
  REQUIRE(p.FateRollLevel(strong, kTamriel, sx, sy) == 30);

  // the two in the forest, the far one out of the ring: 30 and 10
  p.worldState.npcRollLevel = "party-mean";
  REQUIRE(p.FateRollLevel(strong, kTamriel, sx, sy) == 20);
  p.worldState.npcRollLevel = "party-min";
  REQUIRE(p.FateRollLevel(strong, kTamriel, sx, sy) == 10);

  p.worldState.npcRollLevel = "fixed";
  p.worldState.npcRollLevelFixed = 15;
  REQUIRE(p.FateRollLevel(strong, kTamriel, sx, sy) == 15);

  // alone in its ring: the party is the loader itself
  p.worldState.npcRollLevel = "party-mean";
  REQUIRE(p.FateRollLevel(far, kTamriel, Square(kFarAway.x),
                          Square(kFarAway.y)) == 50);
  p.worldState.npcRollLevel = "party-min";
  REQUIRE(p.FateRollLevel(far, kTamriel, Square(kFarAway.x),
                          Square(kFarAway.y)) == 50);

  // a loader with no record yet and nobody near: the players online (30,
  // 10 and 50; the loader counts nothing)
  p.worldState.npcRollLevel = "party-mean";
  REQUIRE(p.FateRollLevel(fresh, kTamriel, Square(kElsewhere.x),
                          Square(kElsewhere.y)) == 30);
  p.worldState.npcRollLevel = "party-min";
  REQUIRE(p.FateRollLevel(fresh, kTamriel, Square(kElsewhere.x),
                          Square(kElsewhere.y)) == 10);
  p.worldState.npcRollLevel = "loader";
  REQUIRE(p.FateRollLevel(fresh, kTamriel, Square(kElsewhere.x),
                          Square(kElsewhere.y)) == 1);

  p.worldState.npcRollLevel = savedRule;
  p.worldState.npcRollLevelFixed = savedFixed;
  for (Networking::UserId user = 0; user < 4; ++user) {
    DoDisconnect(p, user);
  }
  p.DestroyActor(kStrong);
  p.DestroyActor(kWeak);
  p.DestroyActor(kFar);
  p.DestroyActor(kFresh);
}
