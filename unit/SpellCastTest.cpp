#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <thread>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "HitMessage.h"
#include "MsgType.h"
#include "PacketParser.h"
#include "SpellCastMessage.h"

PartOne& GetPartOne();
using namespace std::chrono_literals;

// thuum docs/verbs/spell-cast.md: a player's spell hits only as a cast the
// server recorded: a fire-and-forget cast once, a stream by the time it held
// its target; every cast and its end reach the neighbours reliably.
// Skyrim.esm (thuum lab/esm.py, 2026-10-10): Firebolt 0x12FD0 (SPIT casting
// type 1, fire-and-forget; FireDamageFFAimed 25), Flames 0x12FCD (casting
// type 2, concentration; FireDamageConcAimed 8 a second). A spell's damage
// comes from its records through the effect rule (thuum
// docs/verbs/magic-effects.md), against the victim's recorded 250: a
// Firebolt hit leaves 0.9.
namespace {
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kFlames = 0x00012fcd;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;

// The caster (user 0) at the origin with `spell` in its right hand, and its
// neighbour (user 1) 300 units north with a recorded base health of 250,
// both in Tamriel
MpActor& Duel(PartOne& p, uint32_t spell)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kCaster, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kCaster);
  p.CreateActor(kTarget, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  Equipment eq;
  eq.rightSpell = spell;
  caster.SetEquipment(eq);
  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(target.SetActorValueRecord(record));
  return target;
}

// through the packet path (DoMessage), as a client's message arrives; the
// caster names itself 0x14, as a client may. A cast without its launch (all
// zero), or, with `launch`, the launch its game made at `origin`
void Cast(PartOne& p, uint32_t spell, bool end, bool launch = false,
          NiPoint3 origin = { 0, 0, 0 })
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", kTarget },
                        { "spell", spell },
                        { "isDualCasting", false },
                        { "interruptCast", end },
                        { "castingSource", 1 },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "hasLaunch", launch },
                        { "originX", launch ? origin.x : 0.f },
                        { "originY", launch ? origin.y : 0.f },
                        { "originZ", launch ? origin.z : 0.f },
                        { "launchAngleX", launch ? 0.1f : 0.f },
                        { "launchAngleZ", launch ? 1.57f : 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
}

// the casts the server sent since the last clear: each one's data, and
// whom it went to
std::vector<std::pair<Networking::UserId, nlohmann::json>> Relays(PartOne& p)
{
  std::vector<std::pair<Networking::UserId, nlohmann::json>> out;
  for (auto& m : p.Messages()) {
    if (m.j.contains("t") && m.j["t"] == MsgType::SpellCast) {
      out.emplace_back(m.userId, m.j["data"]);
    }
  }
  return out;
}

void SpellHits(PartOne& p, uint32_t spell)
{
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kTarget;
  hit.data.source = spell;
  p.GetActionListener().OnHit(raw, hit);
}

float Health(PartOne& p)
{
  return p.worldState.GetFormAt<MpActor>(kTarget)
    .GetChangeForm()
    .actorValues.healthPercentage;
}

void Leave(PartOne& p)
{
  p.DestroyActor(kTarget);
  p.DestroyActor(kCaster);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A spell hit without a recorded cast changes nothing",
          "[SpellCast][espm]")
{
  PartOne& p = GetPartOne();
  Duel(p, kFirebolt);

  SpellHits(p, kFirebolt);

  REQUIRE(Health(p) == 1.f);
  Leave(p);
}

TEST_CASE("A fire-and-forget cast covers one hit, and reaches the neighbour "
          "reliably",
          "[SpellCast][espm]")
{
  PartOne& p = GetPartOne();
  Duel(p, kFirebolt);
  p.Messages().clear();

  Cast(p, kFirebolt, false);
  bool relayed = false;
  for (auto& m : p.Messages()) {
    if (m.j.contains("t") && m.j["t"] == 23) {
      // the neighbour's game is told the caster as the server knows it (a
      // 0x14 there would be the neighbour's own player), and the caster's
      // own game is told nothing
      REQUIRE(m.userId == 1);
      REQUIRE(m.reliable);
      REQUIRE(m.j["data"]["caster"].get<uint32_t>() == kCaster);
      // a cast without its launch: the neighbour's game places the spell
      // as it did before
      REQUIRE(m.j["data"]["hasLaunch"] == false);
      REQUIRE(m.j["data"]["originX"] == 0.f);
      REQUIRE(m.j["data"]["originY"] == 0.f);
      REQUIRE(m.j["data"]["originZ"] == 0.f);
      REQUIRE(m.j["data"]["launchAngleX"] == 0.f);
      REQUIRE(m.j["data"]["launchAngleZ"] == 0.f);
      relayed = true;
    }
  }
  REQUIRE(relayed);

  SpellHits(p, kFirebolt);
  REQUIRE(Health(p) == Catch::Approx(0.9f));
  // the same cast again: nothing
  SpellHits(p, kFirebolt);
  REQUIRE(Health(p) == Catch::Approx(0.9f));
  Leave(p);
}

// thuum docs/verbs/spell-cast.md: a target-location cast carries the point
// its caster's game picked along the aim, which no other game can pick for
// a figure; the neighbour's game launches the spell there
TEST_CASE("A cast's launch reaches the neighbour as the caster's game made "
          "it",
          "[SpellCast][espm]")
{
  PartOne& p = GetPartOne();
  Duel(p, kFirebolt);
  p.Messages().clear();

  // at the neighbour's feet, 300 units north of the caster
  Cast(p, kFirebolt, false, true, { 0, 300, 0 });

  const auto relays = Relays(p);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0].first == 1);
  REQUIRE(relays[0].second["hasLaunch"] == true);
  REQUIRE(relays[0].second["originX"] == 0.f);
  REQUIRE(relays[0].second["originY"] == 300.f);
  REQUIRE(relays[0].second["originZ"] == 0.f);
  REQUIRE(relays[0].second["launchAngleX"] == 0.1f);
  REQUIRE(relays[0].second["launchAngleZ"] == 1.57f);
  Leave(p);
}

TEST_CASE("A stream counts the time it held its target, until its end",
          "[SpellCast][espm]")
{
  PartOne& p = GetPartOne();
  Duel(p, kFlames);

  Cast(p, kFlames, false);
  std::this_thread::sleep_for(120ms);
  SpellHits(p, kFlames);
  // 8 a second for 0.12 s to 0.25 s (a slow test machine): 0.96 to 2 of
  // 250, never the whole 8 a report used to count
  const float afterTick = Health(p);
  REQUIRE(afterTick <= 1.f - 0.9f / 250.f);
  REQUIRE(afterTick >= 1.f - 2.05f / 250.f);

  // its end, even with the hand changed since: no more hits
  Equipment eq;
  p.worldState.GetFormAt<MpActor>(kCaster).SetEquipment(eq);
  p.Messages().clear();
  Cast(p, kFlames, true);
  // the end reaches the neighbour as the server resolved it, and not the
  // caster's own game
  bool ended = false;
  for (auto& m : p.Messages()) {
    if (m.j.contains("t") && m.j["t"] == 23) {
      REQUIRE(m.userId == 1);
      REQUIRE(m.j["data"]["interruptCast"].get<bool>());
      REQUIRE(m.j["data"]["caster"].get<uint32_t>() == kCaster);
      ended = true;
    }
  }
  REQUIRE(ended);
  eq.rightSpell = kFlames;
  p.worldState.GetFormAt<MpActor>(kCaster).SetEquipment(eq);
  std::this_thread::sleep_for(60ms);
  SpellHits(p, kFlames);
  REQUIRE(Health(p) == Catch::Approx(afterTick));
  Leave(p);
}

// thuum docs/verbs/spell-cast.md, playtest fourteen: a plugin spell reaches
// by its own effect's projectile, whose ids inside the records count the
// plugin's masters. Dragonborn.esm (thuum lab/esm.py, 2026-10-12):
// DLC2LightningStormRightHand_Miraak, 0x0203D5C2 in its file and 0x0403D5C2
// in the tests' load order, is a concentration spell (40 a second) whose own
// effect DLC2ShockDamageMassConcAimed_Miraak (0x0203D5C3, a dual value
// modifier on health) launches Skyrim.esm's ShockBoltConMassiveAim (range
// 6144). Read as load-order ids, its effect was not found and the spell fell
// back to a touch spell's 512 units, so a target 2000 units off took nothing.
TEST_CASE("A plugin spell reaches by its own effect's projectile",
          "[SpellCast][espm]")
{
  constexpr uint32_t kMiraakStorm = 0x0403d5c2;
  PartOne& p = GetPartOne();
  Duel(p, kMiraakStorm);
  p.worldState.GetFormAt<MpActor>(kTarget).SetPos({ 0, 2000, 0 });

  Cast(p, kMiraakStorm, false);
  std::this_thread::sleep_for(120ms);
  SpellHits(p, kMiraakStorm);
  REQUIRE(Health(p) < 1.f);

  Cast(p, kMiraakStorm, true);
  Leave(p);
}
