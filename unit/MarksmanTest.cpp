#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "HitMessage.h"
#include "PacketParser.h"
#include "PlayerBowShotMessage.h"

PartOne& GetPartOne();
using namespace std::chrono_literals;

// thuum docs/verbs/marksman.md: a shot from the bow the player holds takes
// an arrow, waits for its hit, and reaches the shooter's neighbours to be
// drawn there; a player's arrow hits only as a recorded shot, once.
// Skyrim.esm: LongBow 0x3B562, IronArrow 0x1397D (thuum lab/esm.py).
namespace {
constexpr uint32_t kLongBow = 0x0003b562;
constexpr uint32_t kIronArrow = 0x0001397d;
constexpr uint32_t kShooter = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;

const auto kWorn = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();

// The shooter (user 0) at the origin and its neighbour (user 1) 300 units
// north, both in Tamriel; the shooter holds the long bow and ten iron arrows,
// equipped when `equipped`
MpActor& Range(PartOne& p, bool equipped)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kShooter, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kShooter);
  p.CreateActor(kTarget, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  auto& shooter = p.worldState.GetFormAt<MpActor>(kShooter);
  shooter.AddItem(kLongBow, 1);
  shooter.AddItem(kIronArrow, 10);
  Equipment eq;
  if (equipped) {
    eq.inv.entries.push_back(Inventory::Entry(kLongBow, 1, kWorn));
    eq.inv.entries.push_back(Inventory::Entry(kIronArrow, 10, kWorn));
  }
  shooter.SetEquipment(eq);
  return shooter;
}

void Shoot(PartOne& p)
{
  RawMessageData raw;
  raw.userId = 0;
  PlayerBowShotMessage shot;
  shot.weaponId = kLongBow;
  shot.ammoId = kIronArrow;
  shot.power = 1.f;
  shot.aimAngle = 0.125f;
  shot.aimHeading = 1.5f;
  p.GetActionListener().OnPlayerBowShot(raw, shot);
}

// the shooter's arrow on the target, the weapon's cooldown long past so only
// the shot can refuse it
void ArrowHits(PartOne& p, MpActor& shooter)
{
  shooter.SetLastHitTime(kTarget, std::chrono::steady_clock::now() - 10s);
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kTarget;
  hit.data.source = kLongBow;
  p.GetActionListener().OnHit(raw, hit);
}

std::vector<std::pair<Networking::UserId, nlohmann::json>> Relays(PartOne& p)
{
  std::vector<std::pair<Networking::UserId, nlohmann::json>> out;
  for (auto& m : p.Messages()) {
    if (m.j["t"] == MsgType::ArrowShot) {
      out.emplace_back(m.userId, m.j);
    }
  }
  return out;
}

void Leave(PartOne& p)
{
  p.DestroyActor(kTarget);
  p.DestroyActor(kShooter);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A shot from a held bow takes an arrow and reaches the neighbours",
          "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, true);
  p.Messages().clear();

  Shoot(p);

  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == 9);
  // the neighbour's game is told the shot to draw; the shooter's is not
  const auto relays = Relays(p);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0].first == 1);
  REQUIRE(relays[0].second["idx"] == shooter.GetIdx());
  REQUIRE(relays[0].second["weaponId"] == kLongBow);
  REQUIRE(relays[0].second["ammoId"] == kIronArrow);
  REQUIRE(relays[0].second["power"] == 1.f);
  REQUIRE(relays[0].second["aimAngle"] == 0.125f);
  REQUIRE(relays[0].second["aimHeading"] == 1.5f);

  Leave(p);
}

TEST_CASE("A shot from a bow the player does not hold changes nothing",
          "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, false);
  p.Messages().clear();

  Shoot(p);

  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == 10);
  REQUIRE(Relays(p).empty());

  Leave(p);
}

TEST_CASE("A player's arrow hits only as a recorded shot, once, against the "
          "victim's recorded maximum health",
          "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, true);
  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  // the victim's base health as its game recorded it (the actor-values verb)
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(target.SetActorValueRecord(record));

  // no shot yet: nothing
  ArrowHits(p, shooter);
  REQUIRE(target.GetChangeForm().actorValues.healthPercentage == 1.f);

  // the shot, then its hit: the test's damage formula's 25 out of 250
  Shoot(p);
  ArrowHits(p, shooter);
  REQUIRE(target.GetChangeForm().actorValues.healthPercentage ==
          Catch::Approx(0.9f));

  // the same arrow again: nothing
  ArrowHits(p, shooter);
  REQUIRE(target.GetChangeForm().actorValues.healthPercentage ==
          Catch::Approx(0.9f));

  Leave(p);
}
