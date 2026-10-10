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
// type 1, fire-and-forget), Flames 0x12FCD (casting type 2, concentration).
// The test's damage formula deals 25 for any hit (PartOne_ActivateTest.cpp),
// against the victim's recorded 250: a fire-and-forget hit leaves 0.9.
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

// through the packet path (DoMessage), so the server's relay carries the
// message's bytes as a client's would
void Cast(PartOne& p, uint32_t spell, bool end)
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
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
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
    if (m.userId == 1 && m.j.contains("t") && m.j["t"] == 23) {
      relayed = true;
      REQUIRE(m.reliable);
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

TEST_CASE("A stream counts the time it held its target, until its end",
          "[SpellCast][espm]")
{
  PartOne& p = GetPartOne();
  Duel(p, kFlames);

  Cast(p, kFlames, false);
  std::this_thread::sleep_for(120ms);
  SpellHits(p, kFlames);
  // 25 a second for 0.12 s to 0.25 s (a slow test machine): 3 to 6.25 of
  // 250, never the whole 25 a report used to count
  const float afterTick = Health(p);
  REQUIRE(afterTick <= 1.f - 2.5f / 250.f);
  REQUIRE(afterTick >= 1.f - 6.25f / 250.f);

  // its end, even with the hand changed since: no more hits
  Equipment eq;
  p.worldState.GetFormAt<MpActor>(kCaster).SetEquipment(eq);
  Cast(p, kFlames, true);
  eq.rightSpell = kFlames;
  p.worldState.GetFormAt<MpActor>(kCaster).SetEquipment(eq);
  std::this_thread::sleep_for(60ms);
  SpellHits(p, kFlames);
  REQUIRE(Health(p) == Catch::Approx(afterTick));
  Leave(p);
}
