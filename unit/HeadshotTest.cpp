#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "Appearance.h"
#include "HitMessage.h"
#include "PlayerBowShotMessage.h"

PartOne& GetPartOne();
using namespace std::chrono_literals;

// thuum docs/verbs/headshot.md: the shooter's game reports a head hit as the
// mod's instant death spell on the victim; the server kills when a bow hit
// it claimed on that victim came moments before, the victim's head and hair
// slots are bare by its equipment record, its race is one the rule names,
// and the shooter's base Archery reaches the floor; once per hit. The test
// set has no Headshot Kills.esp, so the rule names Skyrim.esm's Firebolt
// (0x12FD0) as the headshot spell; LongBow 0x3B562, IronArrow 0x1397D, the
// Iron Helmet 0x12E4D (ARMO BOD2 slot 30), NordRace 0x13746 (thuum
// lab/esm.py on sky-srv). Archery is actor value 8 (CommonLibSSE-NG
// include/RE/A/ActorValues.h:16).
namespace {
constexpr uint32_t kHeadshotSpell = 0x00012fd0;
constexpr uint32_t kLongBow = 0x0003b562;
constexpr uint32_t kIronArrow = 0x0001397d;
constexpr uint32_t kIronHelmet = 0x00012e4d;
constexpr uint32_t kNordRace = 0x00013746;
constexpr uint32_t kShooter = 0xff000000;
constexpr uint32_t kVictim = 0xff000001;

const auto kWorn = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();

// the shooter (user 0) at the origin with the bow and arrows worn and base
// Archery `archery`; the victim (user 1) 300 units north, a Nord with 250
// base Health, wearing a helmet when `helmet`
MpActor& Range(PartOne& p, float archery, bool helmet)
{
  p.SetHeadshotSettings(R"({"spells": ["12fd0:Skyrim.esm"], )"
                        R"("races": ["13746:Skyrim.esm"], "minArchery": 60})");
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kShooter, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kShooter);
  p.CreateActor(kVictim, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kVictim);
  auto& shooter = p.worldState.GetFormAt<MpActor>(kShooter);
  shooter.AddItem(kLongBow, 1);
  shooter.AddItem(kIronArrow, 10);
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(kLongBow, 1, kWorn));
  eq.inv.entries.push_back(Inventory::Entry(kIronArrow, 10, kWorn));
  shooter.SetEquipment(eq);
  ActorValueRecord shooterRecord;
  shooterRecord.bases = { { 8, archery } };
  REQUIRE(shooter.SetActorValueRecord(shooterRecord));
  auto& victim = p.worldState.GetFormAt<MpActor>(kVictim);
  Appearance appearance;
  appearance.raceId = kNordRace;
  victim.SetAppearance(&appearance);
  ActorValueRecord victimRecord;
  victimRecord.bases = { { 24, 250.f } };
  REQUIRE(victim.SetActorValueRecord(victimRecord));
  // an actor the test fixture creates starts in the iron set, helmet
  // included (pipeline 1843's facts line: worn 12e46 12e49 12e4b 12e4d), so
  // the bare victim is stripped and the helmeted one wears the helmet alone
  Equipment worn;
  if (helmet) {
    victim.AddItem(kIronHelmet, 1);
    worn.inv.entries.push_back(Inventory::Entry(kIronHelmet, 1, kWorn));
  }
  victim.SetEquipment(worn);
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
  shot.aimAngle = 0.f;
  shot.aimHeading = 1.5f;
  p.GetActionListener().OnPlayerBowShot(raw, shot);
}

void Hits(PartOne& p, MpActor& shooter, uint32_t source)
{
  shooter.SetLastHitTime(kVictim, std::chrono::steady_clock::now() - 10s);
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kVictim;
  hit.data.source = source;
  p.GetActionListener().OnHit(raw, hit);
}

bool VictimDead(PartOne& p)
{
  return p.worldState.GetFormAt<MpActor>(kVictim).IsDead();
}

void Leave(PartOne& p)
{
  p.DestroyActor(kVictim);
  p.DestroyActor(kShooter);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
  p.SetHeadshotSettings("{}");
}
}

TEST_CASE("A headshot on a claimed bow hit kills a bare-headed victim, once",
          "[Headshot][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, 60.f, false);
  // the report alone, no bow hit: nothing
  Hits(p, shooter, kHeadshotSpell);
  REQUIRE(!VictimDead(p));

  Shoot(p);
  Hits(p, shooter, kLongBow);
  REQUIRE(!VictimDead(p));
  Hits(p, shooter, kHeadshotSpell);
  REQUIRE(VictimDead(p));
  Leave(p);
}

TEST_CASE("A headshot on a helmeted victim takes no kill", "[Headshot][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, 60.f, true);
  Shoot(p);
  Hits(p, shooter, kLongBow);
  Hits(p, shooter, kHeadshotSpell);
  REQUIRE(!VictimDead(p));
  Leave(p);
}

TEST_CASE("A headshot by an archer under the floor takes no kill",
          "[Headshot][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, 40.f, false);
  Shoot(p);
  Hits(p, shooter, kLongBow);
  Hits(p, shooter, kHeadshotSpell);
  REQUIRE(!VictimDead(p));
  Leave(p);
}

TEST_CASE("With no headshot settings the spell is no headshot",
          "[Headshot][espm]")
{
  PartOne& p = GetPartOne();
  auto& shooter = Range(p, 60.f, false);
  p.SetHeadshotSettings("{}");
  Shoot(p);
  Hits(p, shooter, kLongBow);
  Hits(p, shooter, kHeadshotSpell);
  REQUIRE(!VictimDead(p));
  Leave(p);
}

// thuum docs/verbs/headshot.md (playtest 32, 2026-10-22): an actor from the
// game files has no appearance, so the rule read race 0 and refused every NPC
// headshot (E_HEADSHOT_RACE on 39fe2, a bare-headed bandit). The race now
// comes from MpActor::GetRaceId, the NPC_ base through its template chain:
// Riverwood's Alvor 0x13482 (base 0x13477, a Nord) loaded from the files,
// no appearance, reads the Nord race and passes the race check.
TEST_CASE("A headshot's race check reads a game-file actor's race from its "
          "NPC_ base, not an appearance it never had",
          "[Headshot][espm]")
{
  PartOne& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& shooter = Range(p, 60.f, false);
  const auto& form = p.worldState.LookupFormById(0x13482);
  REQUIRE(form != nullptr);
  MpActor* alvor = form->AsActor();
  REQUIRE(alvor != nullptr);
  REQUIRE(alvor->GetAppearance() == nullptr);
  REQUIRE(alvor->GetRaceId() == 0x13746);
  Equipment bare;
  alvor->SetEquipment(bare);
  const auto check = p.ClaimHeadshot(shooter, *alvor);
  REQUIRE(check != PartOne::HeadshotCheck::Race);
  Leave(p);
  p.worldState.npcEnabled = false;
}
