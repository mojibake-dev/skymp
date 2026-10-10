#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "ConsoleCommandMessage.h"
#include "HitMessage.h"
#include "PlayerBowShotMessage.h"

PartOne& GetPartOne();
using namespace std::chrono_literals;

// thuum docs/verbs/god-mode.md: TGM and TIM are an admin's; a god actor
// takes no damage from another actor and spends no arrow, and its own game
// is told its god mode; an immortal actor's health stops at one point.
// Skyrim.esm (thuum lab/esm.py): LongBow 0x3B562, IronArrow 0x1397D,
// DB03Poison 0x58CFB (AlchDamageHealthDuration 6 a second for 10 s),
// FortifyHealth01 0x3EAF2 (AlchFortifyHealth 20 for 60 s)
namespace {
constexpr uint32_t kLongBow = 0x0003b562;
constexpr uint32_t kIronArrow = 0x0001397d;
constexpr uint32_t kShooter = 0xff000000;
constexpr uint32_t kGod = 0xff000001;

const auto kWorn = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();

// the shooter (user 0) at the origin with a long bow and arrows worn, and
// the other player (user 1) 300 units north with a recorded base health of
// 100
MpActor& Pair(PartOne& p)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kShooter, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kShooter);
  p.CreateActor(kGod, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kGod);
  auto& shooter = p.worldState.GetFormAt<MpActor>(kShooter);
  shooter.AddItem(kLongBow, 1);
  shooter.AddItem(kIronArrow, 10);
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(kLongBow, 1, kWorn));
  eq.inv.entries.push_back(Inventory::Entry(kIronArrow, 10, kWorn));
  shooter.SetEquipment(eq);
  auto& other = p.worldState.GetFormAt<MpActor>(kGod);
  ActorValueRecord record;
  record.bases = { { 24, 100.f } };
  REQUIRE(other.SetActorValueRecord(record));
  return other;
}

void Console(PartOne& p, Networking::UserId user, const std::string& name)
{
  RawMessageData raw;
  raw.userId = user;
  ConsoleCommandMessage msg;
  msg.data.commandName = name;
  p.GetActionListener().OnConsoleCommand(raw, msg);
}

void Shoot(PartOne& p)
{
  RawMessageData raw;
  raw.userId = 0;
  PlayerBowShotMessage shot;
  shot.weaponId = kLongBow;
  shot.ammoId = kIronArrow;
  shot.power = 1.f;
  p.GetActionListener().OnPlayerBowShot(raw, shot);
}

void ArrowHits(PartOne& p, MpActor& shooter)
{
  shooter.SetLastHitTime(kGod, std::chrono::steady_clock::now() - 10s);
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kGod;
  hit.data.source = kLongBow;
  p.GetActionListener().OnHit(raw, hit);
}

float Health(MpActor& actor)
{
  return actor.GetChangeForm().actorValues.healthPercentage;
}

void Leave(PartOne& p)
{
  p.DestroyActor(kGod);
  p.DestroyActor(kShooter);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
}

TEST_CASE("TGM and TIM are an admin's, and TGM tells the caller's own game",
          "[GodMode]")
{
  PartOne& p = GetPartOne();
  const bool forAll = p.worldState.enableConsoleCommandsForAll;
  p.worldState.enableConsoleCommandsForAll = false;
  auto& god = Pair(p);
  const auto setGodMode = [&](bool on) {
    int n = 0;
    for (auto& m : p.Messages()) {
      n += m.userId == 1 && m.j["t"] == MsgType::SpSnippet &&
        m.j["class"] == "Debug" && m.j["function"] == "SetGodMode" &&
        m.j["arguments"] == nlohmann::json::array({ on });
    }
    return n;
  };

  god.SetStaffRank(0);
  Console(p, 1, "tgm");
  REQUIRE(!god.IsGodMode());

  god.SetStaffRank(2);
  p.Messages().clear();
  Console(p, 1, "tgm");
  REQUIRE(god.IsGodMode());
  REQUIRE(setGodMode(true) == 1);
  Console(p, 1, "ToggleGodMode");
  REQUIRE(!god.IsGodMode());
  REQUIRE(setGodMode(false) == 1);

  Console(p, 1, "tim");
  REQUIRE(god.IsImmortal());

  // the session's: a departure clears both
  Console(p, 1, "tgm");
  REQUIRE(god.IsGodMode());
  DoDisconnect(p, 1);
  REQUIRE(!god.IsGodMode());
  REQUIRE(!god.IsImmortal());
  DoConnect(p, 1);
  p.SetUserActor(1, kGod);

  p.worldState.enableConsoleCommandsForAll = forAll;
  Leave(p);
}

TEST_CASE("A god actor takes no damage from an arrow or a poison, and a god "
          "shooter spends no arrow",
          "[GodMode][espm]")
{
  PartOne& p = GetPartOne();
  auto& god = Pair(p);
  auto& shooter = p.worldState.GetFormAt<MpActor>(kShooter);
  god.SetGodMode(true);

  Shoot(p);
  ArrowHits(p, shooter);
  REQUIRE(Health(god) == Catch::Approx(1.f));

  god.ApplyEffects(0x58CFB, MpActor::EffectsOf(&p.worldState, 0x58CFB), 1.f,
                   kShooter, false);
  god.AdvanceEffects(5.f);
  REQUIRE(Health(god) == Catch::Approx(1.f));

  // a buff still moves the maximum
  god.ApplyEffects(0x3EAF2, MpActor::EffectsOf(&p.worldState, 0x3EAF2), 1.f,
                   0, false);
  REQUIRE(god.GetMaximumValues().health == Catch::Approx(120.f));

  const auto arrows = shooter.GetInventory().GetItemCount(kIronArrow);
  shooter.SetGodMode(true);
  Shoot(p);
  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == arrows);
  shooter.SetGodMode(false);
  Shoot(p);
  REQUIRE(shooter.GetInventory().GetItemCount(kIronArrow) == arrows - 1);

  god.SetGodMode(false);
  Leave(p);
}

TEST_CASE("An immortal actor's health stops at one point", "[GodMode]")
{
  PartOne& p = GetPartOne();
  auto& immortal = Pair(p);
  immortal.SetImmortal(true);

  ActorValues values = immortal.GetActorValues();
  values.healthPercentage = 0.f;
  p.Messages().clear();
  immortal.NetSetPercentages(values, nullptr, std::nullopt);
  REQUIRE(!immortal.IsDead());
  REQUIRE(Health(immortal) == Catch::Approx(0.01f));
  // its game hears the point, never nothing
  for (auto& m : p.Messages()) {
    if (m.userId == 1 && m.j["t"] == MsgType::ChangeValues &&
        m.j["data"].contains("health")) {
      REQUIRE(m.j["data"]["health"].get<float>() > 0.f);
    }
  }

  immortal.SetPercentage(espm::ActorValue::Health, 0.f);
  REQUIRE(!immortal.IsDead());

  immortal.SetImmortal(false);
  immortal.NetSetPercentages(values, nullptr, std::nullopt);
  REQUIRE(immortal.IsDead());
  Leave(p);
}
