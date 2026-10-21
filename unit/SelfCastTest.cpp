#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "HitMessage.h"
#include "MsgType.h"
#include "PlayerBowShotMessage.h"
#include "RestIntentMessage.h"
#include "formulas/TES5DamageFormula.h"

PartOne& GetPartOne();
using namespace std::chrono_literals;

// thuum docs/verbs/self-cast.md: a spell a player casts on itself runs its
// effects on the server, which holds them and their time; Invisibility and
// Etherealize run as states the server's rules read, and the caster's own
// act ends them. Skyrim.esm (thuum lab/esm.py; docs/verbs/magic-effects.md's
// table): Oakflesh 0x5AD5C (ArmorFFSelf0, a peak value modifier on
// DamageResist), Invisibility 0x27EB6 (InvisibillityFFSelf, archetype 11),
// Become Ethereal's first word 0x5F6EB (VoiceMakeEthereal, archetype 41),
// Firebolt 0x12FD0 (aimed); the long bow and arrows as GodModeTest's
namespace {
constexpr uint32_t kOakflesh = 0x0005ad5c;
constexpr uint32_t kInvisibility = 0x00027eb6;
constexpr uint32_t kEthereal = 0x0005f6eb;
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kHealing = 0x00012fcc;
constexpr uint32_t kLongBow = 0x0003b562;
constexpr uint32_t kIronArrow = 0x0001397d;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kShooter = 0xff000001;

const auto kWorn = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();

// the caster (user 0) at the origin with `spell` in its right hand and a
// recorded base health of 100; the shooter (user 1) 300 units north with a
// long bow and arrows worn
MpActor& Pair(PartOne& p, uint32_t spell)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kCaster, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kCaster);
  p.CreateActor(kShooter, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kShooter);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  Equipment eq;
  eq.rightSpell = spell;
  caster.SetEquipment(eq);
  ActorValueRecord record;
  record.bases = { { 24, 100.f } };
  REQUIRE(caster.SetActorValueRecord(record));
  auto& shooter = p.worldState.GetFormAt<MpActor>(kShooter);
  shooter.AddItem(kLongBow, 1);
  shooter.AddItem(kIronArrow, 10);
  Equipment bow;
  bow.inv.entries.push_back(Inventory::Entry(kLongBow, 1, kWorn));
  bow.inv.entries.push_back(Inventory::Entry(kIronArrow, 10, kWorn));
  shooter.SetEquipment(bow);
  return caster;
}

void Hold(MpActor& caster, uint32_t spell)
{
  Equipment eq = caster.GetEquipment();
  eq.rightSpell = spell;
  caster.SetEquipment(eq);
}

// through the packet path, as a client's message arrives; `end` for the
// cast's end message
void Cast(PartOne& p, uint32_t spell, bool end = false)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", 0 },
                        { "spell", spell },
                        { "isDualCasting", false },
                        { "interruptCast", end },
                        { "castingSource", 1 },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "hasLaunch", false },
                        { "originX", 0.f },
                        { "originY", 0.f },
                        { "originZ", 0.f },
                        { "launchAngleX", 0.f },
                        { "launchAngleZ", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
}

// the shooter's arrow on the caster
void ArrowHitsCaster(PartOne& p)
{
  auto& shooter = p.worldState.GetFormAt<MpActor>(kShooter);
  RawMessageData shotRaw;
  shotRaw.userId = 1;
  PlayerBowShotMessage shot;
  shot.weaponId = kLongBow;
  shot.ammoId = kIronArrow;
  shot.power = 1.f;
  p.GetActionListener().OnPlayerBowShot(shotRaw, shot);
  shooter.SetLastHitTime(kCaster, std::chrono::steady_clock::now() - 10s);
  RawMessageData raw;
  raw.userId = 1;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kCaster;
  hit.data.source = kLongBow;
  p.GetActionListener().OnHit(raw, hit);
}

float Health(MpActor& actor)
{
  return actor.GetChangeForm().actorValues.healthPercentage;
}

void Rest(PartOne& p, float hours)
{
  RawMessageData raw;
  raw.userId = 0;
  RestIntentMessage msg;
  msg.hours = hours;
  msg.sleep = false;
  p.GetActionListener().OnRestIntent(raw, msg);
}

bool ToldMagicEffects(PartOne& p, Networking::UserId user)
{
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j["t"] == MsgType::MagicEffects) {
      return true;
    }
  }
  return false;
}

void Leave(PartOne& p)
{
  p.DestroyActor(kShooter);
  p.DestroyActor(kCaster);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A Self spell's cast runs its effects on its caster on the server, "
          "and its armor counts in the damage it takes",
          "[SelfCast][espm]")
{
  PartOne& p = GetPartOne();
  // the engine's formula, which counts the target's armor
  p.SetDamageFormula(std::make_unique<TES5DamageFormula>());
  auto& caster = Pair(p, kOakflesh);

  // an arrow without Oakflesh
  ArrowHitsCaster(p);
  const float bare = 1.f - Health(caster);
  REQUIRE(bare > 0.f);
  caster.SetPercentages({ 1.f, 1.f, 1.f });

  p.Messages().clear();
  Cast(p, kOakflesh);
  REQUIRE(caster.GetEffectModifier(espm::ActorValue::DamageResist) > 0.f);
  // the neighbour's game hears the caster's effects
  REQUIRE(ToldMagicEffects(p, 1));

  // the same arrow takes less
  ArrowHitsCaster(p);
  const float armored = 1.f - Health(caster);
  REQUIRE(armored > 0.f);
  REQUIRE(armored < bare);

  // an aimed spell runs nothing on its caster
  const auto before = caster.GetRunningEffects().size();
  Hold(caster, kFirebolt);
  Cast(p, kFirebolt);
  REQUIRE(caster.GetRunningEffects().size() == before);
  Leave(p);
}

// Skyrim.esm (thuum lab/esm.py on sky-srv, 2026-10-17): Fireball 0x1C789's
// first effect FireDamageFFAimedArea 0x1CEA1 has an area of 15 and a
// detrimental record; Oakflesh 0x5AD5C's effects have no area
TEST_CASE("A Self burst's hostile area effect is not its caster's",
          "[SelfCast][espm]")
{
  PartOne& p = GetPartOne();
  constexpr uint32_t kFireball = 0x0001c789;
  const auto fireball = MpActor::EffectsOf(&p.worldState, kFireball);
  REQUIRE(!fireball.empty());
  REQUIRE(fireball.front().areaOfEffect > 0);
  REQUIRE(MpActor::IsHostileArea(&p.worldState, fireball.front()));
  const auto oakflesh = MpActor::EffectsOf(&p.worldState, kOakflesh);
  REQUIRE(!oakflesh.empty());
  for (const auto& effect : oakflesh) {
    REQUIRE(!MpActor::IsHostileArea(&p.worldState, effect));
  }
}

TEST_CASE("Invisibility runs as a state, and the caster's next act ends it",
          "[SelfCast][espm]")
{
  PartOne& p = GetPartOne();
  auto& caster = Pair(p, kInvisibility);
  Cast(p, kInvisibility);
  REQUIRE(caster.HasRunningEffectOfType(espm::MGEF::EffectType::Invisibility));
  // changing no value
  REQUIRE(caster.GetMaximumValues().health == Catch::Approx(100.f));

  Hold(caster, kFirebolt);
  Cast(p, kFirebolt);
  REQUIRE(
    !caster.HasRunningEffectOfType(espm::MGEF::EffectType::Invisibility));
  Leave(p);
}

TEST_CASE("A recast of Invisibility keeps its caster invisible, as the "
          "engine spares the archetype its own spell carries",
          "[SelfCast][espm]")
{
  // thuum docs/verbs/self-cast.md (playtest nineteen: the recast made the
  // figure visible on the other screen): the engine's DispelAlteredStates
  // passes the cast spell's own archetype as its exception, so the recast's
  // copy replaces the running one; the server ended it and told every game
  PartOne& p = GetPartOne();
  auto& caster = Pair(p, kInvisibility);
  Cast(p, kInvisibility);
  REQUIRE(caster.HasRunningEffectOfType(espm::MGEF::EffectType::Invisibility));
  Cast(p, kInvisibility);
  REQUIRE(caster.HasRunningEffectOfType(espm::MGEF::EffectType::Invisibility));

  // another spell still ends it
  Hold(caster, kFirebolt);
  Cast(p, kFirebolt);
  REQUIRE(
    !caster.HasRunningEffectOfType(espm::MGEF::EffectType::Invisibility));
  Leave(p);
}

TEST_CASE("A bow's release ends its shooter's concealment, as the engine's "
          "release does",
          "[SelfCast][espm]")
{
  // thuum docs/verbs/self-cast.md: ArrowReleaseHandler calls
  // DispelAlteredStates after the shot, with no exception
  PartOne& p = GetPartOne();
  Pair(p, kInvisibility);
  auto& shooter = p.worldState.GetFormAt<MpActor>(kShooter);
  shooter.ApplyEffects(kInvisibility,
                       MpActor::EffectsOf(&p.worldState, kInvisibility), 1.f,
                       kShooter, false);
  REQUIRE(
    shooter.HasRunningEffectOfType(espm::MGEF::EffectType::Invisibility));
  ArrowHitsCaster(p);
  REQUIRE(
    !shooter.HasRunningEffectOfType(espm::MGEF::EffectType::Invisibility));
  Leave(p);
}

TEST_CASE("An ethereal player takes no damage from another's arrow",
          "[SelfCast][espm]")
{
  PartOne& p = GetPartOne();
  auto& caster = Pair(p, kOakflesh);
  caster.ApplyEffects(kEthereal, MpActor::EffectsOf(&p.worldState, kEthereal),
                      1.f, kCaster, false);
  REQUIRE(caster.HasRunningEffectOfType(espm::MGEF::EffectType::Etherealize));
  ArrowHitsCaster(p);
  REQUIRE(Health(caster) == Catch::Approx(1.f));
  // the shooter's own attack ended nothing of the caster's
  REQUIRE(caster.HasRunningEffectOfType(espm::MGEF::EffectType::Etherealize));
  Leave(p);
}

TEST_CASE("A rest runs the player's effects for its time", "[SelfCast][espm]")
{
  PartOne& p = GetPartOne();
  auto& caster = Pair(p, kInvisibility);
  Cast(p, kInvisibility);
  REQUIRE(caster.HasRunningEffectOfType(espm::MGEF::EffectType::Invisibility));
  Rest(p, 1.f);
  REQUIRE(
    !caster.HasRunningEffectOfType(espm::MGEF::EffectType::Invisibility));
  Leave(p);
}

TEST_CASE("A Self concentration spell runs on the server as a stream for the "
          "cast's span, and its end message ends it; a fire-and-forget Self "
          "spell's end message ends nothing",
          "[SelfCast][espm]")
{
  // thuum docs/verbs/self-cast.md (train-31; playtest twenty-eight's heals):
  // Healing (Skyrim.esm 0x12FCC, Self, concentration, 10 Health a second;
  // UESP Skyrim:Healing) heals its caster on the server while the cast
  // holds, so the caster's own game's rise is not cropped away; Oakflesh's
  // end message leaves Oakflesh running its recorded time
  PartOne& p = GetPartOne();
  auto& caster = Pair(p, kHealing);
  caster.SetPercentages({ 0.5f, 1.f, 1.f });
  const auto before = caster.GetRunningEffects().size();

  Cast(p, kHealing);
  REQUIRE(caster.GetRunningEffects().size() == before + 1);
  REQUIRE(caster.GetRunningEffects().back().source == kHealing);
  caster.AdvanceEffects(2.f);
  const float healed = Health(caster);
  REQUIRE(healed > 0.5f);
  REQUIRE(healed == Catch::Approx(0.7f).margin(0.1f));

  // the end message: the stream ends, nothing more is healed
  Cast(p, kHealing, true);
  REQUIRE(caster.GetRunningEffects().size() == before);
  caster.AdvanceEffects(2.f);
  REQUIRE(Health(caster) == Catch::Approx(healed).margin(0.001f));

  // a recast restarts one stream, not two
  Cast(p, kHealing);
  Cast(p, kHealing);
  REQUIRE(caster.GetRunningEffects().size() == before + 1);
  Cast(p, kHealing, true);
  REQUIRE(caster.GetRunningEffects().size() == before);

  Hold(caster, kOakflesh);
  Cast(p, kOakflesh);
  REQUIRE(caster.GetEffectModifier(espm::ActorValue::DamageResist) > 0.f);
  Cast(p, kOakflesh, true);
  REQUIRE(caster.GetEffectModifier(espm::ActorValue::DamageResist) > 0.f);
  Leave(p);
}
