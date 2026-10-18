#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "HitMessage.h"
#include "MsgType.h"
#include "SpellCastMessage.h"

PartOne& GetPartOne();

// thuum docs/verbs/second-hand.md: a hit by a spell the caster's own cast
// reaches through the records counts as the caster's, bounded by that
// cast. Skyrim.esm (thuum lab/esm.py on sky-srv, 2026-10-17): Flame Cloak
// is FlameCloakLeftHand 0xDE511, whose effect FireCloakFFSelf 0x3AE9E is a
// Cloak with the associated item (DATA 0x08) FlameCloakDmg 0x2B385, a
// concentration spell of FireDamageConcAimedCloak at 8 a second; Firebolt
// 0x12FD0 reaches nothing. Against the victim's recorded 250 Health a
// first proc report counts one cadence, half a second (wire-rules
// REACHED_TICK_MS): 4 damage, 0.984 left.
namespace {
constexpr uint32_t kFlameCloak = 0x000de511;
constexpr uint32_t kFlameCloakDmg = 0x0002b385;
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;

// the caster (user 0) at the origin with `spell` in its right hand, its
// neighbour (user 1) 300 units north with a recorded base health of 250
void Duel(PartOne& p, uint32_t spell)
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
}

void Cast(PartOne& p, uint32_t spell)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", 0 },
                        { "spell", spell },
                        { "isDualCasting", false },
                        { "interruptCast", false },
                        { "castingSource", 1 },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
}

void Hits(PartOne& p, uint32_t spell)
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

TEST_CASE("A cloak's spell counts as its caster's hit, by the time it held",
          "[SecondHand][espm]")
{
  PartOne& p = GetPartOne();
  Duel(p, kFlameCloak);

  // the cloak's spell with no cloak cast: nothing
  Hits(p, kFlameCloakDmg);
  REQUIRE(Health(p) == Catch::Approx(1.f));

  Cast(p, kFlameCloak);
  Hits(p, kFlameCloakDmg);
  REQUIRE(Health(p) == Catch::Approx(0.984f));
  // the same report again counts the time since the first, which is the
  // milliseconds this test took between the two calls (36 on a Windows
  // runner, build 39088647542), never another cadence: under 4 more of 250
  Hits(p, kFlameCloakDmg);
  REQUIRE(Health(p) <= 0.984f + 1e-4f);
  REQUIRE(Health(p) > 0.984f - 4.f / 250.f);
  Leave(p);
}

TEST_CASE("A spell no recorded cast reaches is refused", "[SecondHand][espm]")
{
  PartOne& p = GetPartOne();
  Duel(p, kFirebolt);
  Cast(p, kFirebolt);
  Hits(p, kFlameCloakDmg);
  REQUIRE(Health(p) == Catch::Approx(1.f));
  Leave(p);
}
