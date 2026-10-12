#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "HitMessage.h"

PartOne& GetPartOne();
using namespace std::chrono_literals;

// thuum docs/verbs/blocking.md: whether a hit was blocked, and the share of
// its damage the block stops, are the server's (wire-rules block): the
// target's recorded block and its facing, a shield for an arrow, the game's
// formula (thuum ghidra/notes/block-formula.md); the attacker's own verdict
// is not read. Skyrim.esm (thuum
// lab/esm.py, 2026-10-10): ArmorIronShield 0x12EB6 (BOD2 0x200, the shield's
// slot; rating 20), IronSword 0x12EB7, LongBow 0x3B562. The test's damage
// formula deals 25 for any hit (PartOne_ActivateTest.cpp), against the
// target's recorded 250.
namespace {
constexpr uint32_t kAggressor = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;
constexpr uint32_t kIronSword = 0x00012eb7;
constexpr uint32_t kIronShield = 0x00012eb6;

Inventory::ExtraData Worn(bool left)
{
  Inventory::ExtraData extra;
  if (left) {
    extra.wornLeft = true;
  } else {
    extra.worn_ = true;
  }
  return extra;
}

// The aggressor (user 0) at the origin facing north with an Iron Sword; the
// target (user 1) 100 units north facing it, its health 250 and its Block
// 15 recorded, an Iron Shield in its left hand when `shield`
MpActor& Pair(PartOne& p, bool shield)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kAggressor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kAggressor);
  p.CreateActor(kTarget, { 0, 100, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  auto& aggressor = p.worldState.GetFormAt<MpActor>(kAggressor);
  aggressor.AddItem(kIronSword, 1);
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(kIronSword, 1, Worn(false)));
  aggressor.SetEquipment(eq);

  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  target.SetAngle({ 0.f, 0.f, 180.f });
  ActorValueRecord record;
  record.bases = { { 9, 15.f }, { 24, 250.f } };
  REQUIRE(target.SetActorValueRecord(record));
  if (shield) {
    target.AddItem(kIronShield, 1);
    Equipment teq;
    teq.inv.entries.push_back(Inventory::Entry(kIronShield, 1, Worn(true)));
    target.SetEquipment(teq);
  }
  return target;
}

float Health(MpActor& target)
{
  return target.GetChangeForm().actorValues.healthPercentage;
}

// a sword hit as the aggressor's game reports it, with its own verdict
void Hit(PartOne& p, bool claimsBlocked)
{
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kTarget;
  hit.data.source = kIronSword;
  hit.data.isHitBlocked = claimsBlocked;
  p.GetActionListener().OnHit(raw, hit);
}

void Leave(PartOne& p)
{
  p.DestroyActor(kTarget);
  p.DestroyActor(kAggressor);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A blocker facing its attacker takes the game's blocked share",
          "[Blocking][espm]")
{
  PartOne& p = GetPartOne();
  auto& target = Pair(p, true);
  target.SetIsBlockActive(true);

  // the attacker's game saw no block: the server's records decide
  Hit(p, false);
  // 25 x (1 - (0.45 + 0.2 / 100 x 20 x (1 + 15 x 1.5 / 100))) = 12.525
  REQUIRE(Health(target) == Catch::Approx(1.f - 12.525f / 250.f));
  Leave(p);
}

TEST_CASE("A blocker hit from behind takes the whole hit, whatever the "
          "attacker's game says",
          "[Blocking][espm]")
{
  PartOne& p = GetPartOne();
  auto& target = Pair(p, true);
  target.SetIsBlockActive(true);
  target.SetAngle({ 0.f, 0.f, 0.f });

  Hit(p, true);
  REQUIRE(Health(target) == Catch::Approx(1.f - 25.f / 250.f));
  Leave(p);
}

TEST_CASE("A target not blocking takes the whole hit, whatever the "
          "attacker's game says",
          "[Blocking][espm]")
{
  PartOne& p = GetPartOne();
  auto& target = Pair(p, true);

  Hit(p, true);
  REQUIRE(Health(target) == Catch::Approx(1.f - 25.f / 250.f));
  Leave(p);
}

TEST_CASE("A block with a weapon stops the weapon formula's share",
          "[Blocking][espm]")
{
  PartOne& p = GetPartOne();
  auto& target = Pair(p, false);
  target.SetIsBlockActive(true);

  Hit(p, false);
  // the Iron Sword's 7: 25 x (1 - (0.3 + 0.2 / 100 x 7 x 1.225)) = 17.07125
  REQUIRE(Health(target) == Catch::Approx(1.f - 17.07125f / 250.f));
  Leave(p);
}
