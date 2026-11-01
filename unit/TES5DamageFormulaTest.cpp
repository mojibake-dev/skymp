#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>

#include "GetBaseActorValues.h"
#include "HitData.h"
#include "PacketParser.h"
#include "formulas/TES5DamageFormula.h"
#include "libespm/Loader.h"

namespace {
const auto kExtraWornTrue = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();
const auto kExtraWornFalse = [] {
  Inventory::ExtraData extra;
  extra.worn_ = false;
  return extra;
}();
}

PartOne& GetPartOne();
extern espm::Loader l;
using namespace std::chrono_literals;

TEST_CASE("Formula takes weapon damage into account", "[TES5DamageFormula]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);

  ac.SetEquipment(Equipment());

  RawMessageData rawMsgData;
  rawMsgData.userId = 0;
  HitData hitData;
  hitData.target = 0x14;
  hitData.aggressor = 0x14;
  hitData.source = 0x0001397E; // iron dagger 4 damage

  TES5DamageFormula formula{};
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 4.0f);

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

// thuum docs/verbs/marksman.md: a bow's or crossbow's arrow adds its own
// damage to the weapon's. Skyrim.esm: LongBow 0x3B562 deals 6, IronArrow
// 0x1397D 8 (thuum lab/esm.py, 2026-10-09)
TEST_CASE("A bow's hit adds the damage of the arrow its shooter has equipped",
          "[TES5DamageFormula]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);

  const uint32_t longBow = 0x0003b562;
  const uint32_t ironArrow = 0x0001397d;
  const uint32_t ironDagger = 0x0001397e;
  HitData hitData;
  hitData.target = 0x14;
  hitData.aggressor = 0x14;
  hitData.source = longBow;
  TES5DamageFormula formula{};

  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(longBow, 1, kExtraWornTrue));
  eq.inv.entries.push_back(Inventory::Entry(ironArrow, 10, kExtraWornTrue));
  ac.SetEquipment(eq);
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 14.0f);

  // no arrow equipped: the bow's own
  eq.inv.entries[1] = Inventory::Entry(ironArrow, 10, kExtraWornFalse);
  ac.SetEquipment(eq);
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 6.0f);

  // an arrow adds to a bow only
  eq.inv.entries[1] = Inventory::Entry(ironArrow, 10, kExtraWornTrue);
  eq.inv.entries.push_back(Inventory::Entry(ironDagger, 1, kExtraWornTrue));
  ac.SetEquipment(eq);
  hitData.source = ironDagger;
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 4.0f);

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("Damage is reduced based on target's armor", "[TES5DamageFormula]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);

  RawMessageData rawMsgData;
  rawMsgData.userId = 0;
  HitData hitData;
  hitData.target = 0x14;
  hitData.aggressor = 0x14;
  hitData.source = 0x0001397E; // iron dagger 4 damage

  // 77382 = 0x12e46: Iron Gauntlets, rating = 10
  // 77387 = 0x12e4b: Iron Boots, rating = 10
  // 77389 = 0x12e4d: Iron Helmet, rating = 15
  // Total rating for worn armor: 10 + 10 = 20

  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(77382, 1, kExtraWornTrue));
  eq.inv.entries.push_back(Inventory::Entry(77387, 1, kExtraWornTrue));
  eq.inv.entries.push_back(Inventory::Entry(77389, 1, kExtraWornFalse));
  ac.SetEquipment(eq);

  TES5DamageFormula formula{};
  // 4 * 0.01 * (100 - 20 * .12) = 3,904
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 3.903999805f);

  auto repeatativeEntry = Inventory::Entry(77382, 1, kExtraWornTrue);
  Equipment eq2;

  for (int i = 0; i < 70; i++) {
    eq2.inv.entries.push_back(repeatativeEntry);
  }

  // Total rating for worn armor: 10 * 70 = 700
  ac.SetEquipment(eq2);

  // Armor rating is 700 * 0.12% = 84%
  // But fMaxArmorRating = 80%
  // 4 * 0.01 * (100 - 80) = 4 * 0.2 = 0.8
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 0.7999999523f);

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("Formula is race-dependent for unarmed attack",
          "[TES5DamageFormula]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  // Nord bu default
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  ac.SetEquipment(Equipment());

  RawMessageData rawMsgData;
  rawMsgData.userId = 0;
  HitData hitData;
  hitData.target = 0x14;
  hitData.aggressor = 0x14;
  hitData.source = 0x1f4; // unarmed attack

  {
    TES5DamageFormula formula{};
    REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 4.0f);
  }

  Appearance appearance;
  appearance.raceId = 0x13745; // KhajiitRace
  ac.SetAppearance(&appearance);
  ac.SetPercentages({ 1, 1, 1 });

  {
    TES5DamageFormula formula{};
    REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 10.0f);
  }

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("A sneak attack's multiplier is the game's for the weapon type",
          "[TES5DamageFormula]")
{
  // thuum docs/verbs/sneak-damage.md: Skyrim.esm's fCombatSneak1HDaggerMult
  // and fCombatSneak1HSwordMult are 3, fCombatSneakHandMult 2 (lab/esm.py,
  // 2026-10-04); SkyMP's formula applied a flat 1.3
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  ac.SetEquipment(Equipment());

  const auto damage = [&](uint32_t source, bool sneak) {
    HitData hitData;
    hitData.target = 0x14;
    hitData.aggressor = 0x14;
    hitData.source = source;
    hitData.isSneakAttack = sneak;
    TES5DamageFormula formula{};
    return formula.CalculateDamage(ac, ac, hitData);
  };

  const uint32_t ironDagger = 0x0001397E, ironSword = 0x00012EB7,
                 unarmed = 0x1F4;
  REQUIRE_THAT(
    damage(ironDagger, true),
    Catch::Matchers::WithinAbs(damage(ironDagger, false) * 3.f, 1e-4));
  REQUIRE_THAT(
    damage(ironSword, true),
    Catch::Matchers::WithinAbs(damage(ironSword, false) * 3.f, 1e-4));
  REQUIRE_THAT(damage(unarmed, true),
               Catch::Matchers::WithinAbs(damage(unarmed, false) * 2.f, 1e-4));

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

// thuum docs/verbs/perks.md (the second half): Armsman00 0xbabe4 multiplies a
// one-handed weapon's damage by 1.2 (Skyrim.esm's kModAttackDamage entry,
// lab/esm.py 2026-11-01), Armsman20 0x79343 by 1.4 with the rank below
// excluded; a bow carries no one-handed keyword. Iron sword 0x12eb7 deals 7,
// long bow 0x3b562 6 (no arrow equipped here).
TEST_CASE("A held weapon perk multiplies the hit by its rank, for its "
          "school's weapons alone",
          "[TES5DamageFormula]")
{
  PartOne& p = GetPartOne();
  p.SetPerksEnabled(true);
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  ac.SetEquipment(Equipment());
  HitData hitData;
  hitData.target = 0x14;
  hitData.aggressor = 0x14;
  hitData.source = 0x12eb7;
  TES5DamageFormula formula{};
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 7.0f);

  ActorValueRecord record;
  record.level = 5;
  record.perks = { 0xbabe4 };
  ac.SetActorValueRecord(record);
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == Catch::Approx(8.4f));

  record.perks = { 0x79343, 0xbabe4 };
  ac.SetActorValueRecord(record);
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == Catch::Approx(9.8f));

  hitData.source = 0x3b562;
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 6.0f);

  // the switch off: the number as before
  p.SetPerksEnabled(false);
  hitData.source = 0x12eb7;
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == 7.0f);
  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

// Juggernaut00 0xbcd2a: a heavy piece's rating times 1.2 (kModArmorRating
// with the piece's ArmorHeavy keyword 0x6bbd2 and not the shield's); Iron
// Gauntlets 0x12e46 rate 10 and are heavy. 4 * 0.01 * (100 - 10 * 0.12) =
// 3.952 bare, 4 * 0.01 * (100 - 12 * 0.12) = 3.9424 with the perk.
TEST_CASE("A held armor perk multiplies a worn piece's rating",
          "[TES5DamageFormula]")
{
  PartOne& p = GetPartOne();
  p.SetPerksEnabled(true);
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(0x12e46, 1, kExtraWornTrue));
  ac.SetEquipment(eq);
  HitData hitData;
  hitData.target = 0x14;
  hitData.aggressor = 0x14;
  hitData.source = 0x0001397E; // iron dagger 4 damage
  TES5DamageFormula formula{};
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == Catch::Approx(3.952f));

  ActorValueRecord record;
  record.level = 5;
  record.perks = { 0xbcd2a };
  ac.SetActorValueRecord(record);
  REQUIRE(formula.CalculateDamage(ac, ac, hitData) == Catch::Approx(3.9424f));
  p.SetPerksEnabled(false);
  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}
