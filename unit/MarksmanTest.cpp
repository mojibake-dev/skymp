#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <memory>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>

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
// a shooter of its own for the stale-hands case: the once-per-ten-seconds
// line is kept per shooter across the process, and 0xff000000 shoots in
// other files' cases too
constexpr uint32_t kStaleShooter = 0xff000e20;

const auto kWorn = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();

// The shooter (user 0) at the origin and its neighbour (user 1) 300 units
// north, both in Tamriel; the shooter is given the long bow and ten iron
// arrows (on top of the arrows its base starts with), equipped when
// `equipped`
MpActor& Range(PartOne& p, bool equipped, uint32_t shooterId = kShooter)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(shooterId, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, shooterId);
  p.CreateActor(kTarget, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  auto& shooter = p.worldState.GetFormAt<MpActor>(shooterId);
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

// a shot without its launch (its game saw none in time: all zero), or, with
// `launch`, the launch its game made from `origin`
void Shoot(PartOne& p, float power = 1.f, bool launch = false,
           NiPoint3 origin = { 0, 0, 0 })
{
  RawMessageData raw;
  raw.userId = 0;
  PlayerBowShotMessage shot;
  shot.idx = p.serverState.ActorByUser(raw.userId)->GetIdx();
  shot.weaponId = kLongBow;
  shot.ammoId = kIronArrow;
  shot.power = power;
  shot.aimAngle = 0.125f;
  shot.aimHeading = 1.5f;
  shot.hasLaunch = launch;
  shot.originX = launch ? origin.x : 0.f;
  shot.originY = launch ? origin.y : 0.f;
  shot.originZ = launch ? origin.z : 0.f;
  shot.launchAngleX = launch ? 0.1f : 0.f;
  shot.launchAngleZ = launch ? 1.57f : 0.f;
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

void Leave(PartOne& p, uint32_t shooterId = kShooter)
{
  p.DestroyActor(kTarget);
  p.DestroyActor(shooterId);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}

// the default logger's lines at info and above while the guard lives
// (PartOneTest's shape), restored at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("marksman-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~LogCapture() { spdlog::set_default_logger(old); }
  size_t Count(const std::string& line) const
  {
    const std::string log = oss.str();
    size_t n = 0;
    for (auto at = log.find(line); at != std::string::npos;
         at = log.find(line, at + line.size())) {
      ++n;
    }
    return n;
  }
};
}

TEST_CASE("A shot from a held bow takes an arrow and reaches the neighbours",
          "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, true);
  const auto arrows = shooter.GetInventory().GetItemCount(kIronArrow);
  p.Messages().clear();

  Shoot(p);

  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == arrows - 1);
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
  // a shot without its launch: the figure's arrow leaves along the aim
  REQUIRE(relays[0].second["hasLaunch"] == false);
  REQUIRE(relays[0].second["originX"] == 0.f);
  REQUIRE(relays[0].second["originY"] == 0.f);
  REQUIRE(relays[0].second["originZ"] == 0.f);
  REQUIRE(relays[0].second["launchAngleX"] == 0.f);
  REQUIRE(relays[0].second["launchAngleZ"] == 0.f);

  Leave(p);
}

TEST_CASE("A shot's launch reaches the neighbours as the shooter's game made "
          "it",
          "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, true);
  const auto arrows = shooter.GetInventory().GetItemCount(kIronArrow);
  p.Messages().clear();

  // from the bow, a little ahead of the shooter's feet and above them
  Shoot(p, 1.f, true, { 10, 20, 120 });

  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == arrows - 1);
  const auto relays = Relays(p);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0].first == 1);
  REQUIRE(relays[0].second["hasLaunch"] == true);
  REQUIRE(relays[0].second["originX"] == 10.f);
  REQUIRE(relays[0].second["originY"] == 20.f);
  REQUIRE(relays[0].second["originZ"] == 120.f);
  REQUIRE(relays[0].second["launchAngleX"] == 0.1f);
  REQUIRE(relays[0].second["launchAngleZ"] == 1.57f);

  Leave(p);
}

TEST_CASE("A shot launched far from the shooter changes nothing",
          "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, true);
  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(target.SetActorValueRecord(record));
  const auto arrows = shooter.GetInventory().GetItemCount(kIronArrow);
  p.Messages().clear();

  // 600 units off where the server has the shooter (E_SHOT_ORIGIN): no
  // arrow taken, nothing relayed, no shot recorded for a hit to claim
  Shoot(p, 1.f, true, { 0, 600, 0 });

  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == arrows);
  REQUIRE(Relays(p).empty());
  ArrowHits(p, shooter);
  REQUIRE(target.GetChangeForm().actorValues.healthPercentage == 1.f);

  Leave(p);
}

// thuum docs/verbs/marksman.md (playtest 37's finding 45): the record's
// hands are the client's last accepted report and lag it (a report refused
// for one entry, the hands a login re-dresses); the inventory is the
// server's own, so a bow and an arrow held there make the shot
TEST_CASE("A shot from a bow the player's hands lack but its inventory holds "
          "is accepted on the inventory, said once, and its arrow's hit lands",
          "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, false, kStaleShooter);
  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(target.SetActorValueRecord(record));
  REQUIRE(shooter.GetEquipment().inv.entries.empty());
  const auto arrows = shooter.GetInventory().GetItemCount(kIronArrow);
  p.Messages().clear();

  {
    LogCapture log;
    Shoot(p);
    Shoot(p);
    // the allowance is said once per shooter per ten seconds
    REQUIRE(log.Count("hands stale: shot from 3b562 accepted on the "
                      "inventory") == 1);
    REQUIRE(log.Count("E_SHOT_NO_BOW") == 0);
    REQUIRE(log.Count("E_SHOT_NO_ARROW") == 0);
  }

  // both shots taken: an arrow each, a relay each
  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == arrows - 2);
  REQUIRE(Relays(p).size() == 2);

  // the hit claims a recorded shot through the same allowance (OnHit reads
  // the record's hands too): the test's damage formula's 25 out of 250
  ArrowHits(p, shooter);
  REQUIRE(target.GetChangeForm().actorValues.healthPercentage ==
          Catch::Approx(0.9f));

  Leave(p, kStaleShooter);
}

TEST_CASE("A shot from a bow the player's inventory lacks changes nothing",
          "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, false);
  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(target.SetActorValueRecord(record));
  shooter.RemoveItem(kLongBow, shooter.GetInventory().GetItemCount(kLongBow),
                     nullptr);
  REQUIRE(shooter.GetInventory().GetItemCount(kLongBow) == 0);
  const auto arrows = shooter.GetInventory().GetItemCount(kIronArrow);
  p.Messages().clear();

  // E_SHOT_NO_BOW: no arrow taken, nothing relayed, no shot for a hit
  Shoot(p);

  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == arrows);
  REQUIRE(Relays(p).empty());
  ArrowHits(p, shooter);
  REQUIRE(target.GetChangeForm().actorValues.healthPercentage == 1.f);

  Leave(p);
}

TEST_CASE("A shot whose arrow the player's inventory lacks changes nothing",
          "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, false);
  shooter.RemoveItem(kIronArrow,
                     shooter.GetInventory().GetItemCount(kIronArrow), nullptr);
  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == 0);
  REQUIRE(shooter.GetInventory().GetItemCount(kLongBow) > 0);
  p.Messages().clear();

  // E_SHOT_NO_ARROW: nothing relayed
  Shoot(p);

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

  // the shot, then its hit, arriving together as after a resend (300 units
  // in no time, within the rule's skew): the test's damage formula's 25 out
  // of 250
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

TEST_CASE("A player's arrow hits at its shot's draw power", "[Marksman][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, true);
  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(target.SetActorValueRecord(record));

  // a half draw: half the test's damage formula's 25 out of 250
  Shoot(p, 0.5f);
  ArrowHits(p, shooter);
  REQUIRE(target.GetChangeForm().actorValues.healthPercentage ==
          Catch::Approx(0.95f));

  Leave(p);
}
