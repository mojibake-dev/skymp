#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActorValueRecord.h"
#include "HitData.h"
#include "MsgType.h"
#include "formulas/TES5DamageFormula.h"

PartOne& GetPartOne();

// One difficulty, the server's, scales every hit an actor takes, as the
// engine's DamageActor (37335) does on each game: by fDiffMultHPToPC* when
// the victim is the player, fDiffMultHPByPC* for any other victim, the
// entry the player's difficulty names (26503 indexes the two tables by
// PlayerCharacter::GetGameStatsData().difficulty, CommonLibSSE-NG
// include/RE/P/PlayerCharacter.h:399). Legendary's are Update.esm's
// fDiffMultHPToPCL 3 (over the executable's 2.5) and the executable's
// fDiffMultHPByPCL 0.25; Adept's are 1 in both. A spell's Health damage
// takes the same multiplier: ValueModifierEffect::ModifyActorValue (35086)
// sends a Health loss through DamageActor.
namespace {
constexpr uint32_t kPlayer = 0xff000000;
// a Bleak Falls Temple bandit, an NPC from the game files
constexpr uint32_t kBandit = 0x39fe4;
// Skyrim.esm IronDagger, 4 damage
constexpr uint32_t kIronDagger = 0x0001397e;
// Skyrim.esm Firebolt: FireDamageFFAimed 0x12f03 at 25, no duration
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint8_t kAdept = 2;
constexpr uint8_t kLegendary = 5;
constexpr float kLegendaryTaken = 3.f;
constexpr float kLegendaryDealt = 0.25f;

MpActor& Player(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kPlayer, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kPlayer);
  auto& player = p.worldState.GetFormAt<MpActor>(kPlayer);
  player.SetEquipment(Equipment());
  ActorValueRecord record;
  record.bases = { { 24, 1000.f } };
  player.SetActorValueRecord(record);
  return player;
}

MpActor& Bandit(PartOne& p)
{
  p.worldState.npcSettings["Skyrim.esm"].spawnInInterior = true;
  p.worldState.npcEnabled = true;
  return p.worldState.GetFormAt<MpActor>(kBandit);
}

void Leave(PartOne& p)
{
  p.DestroyActor(kPlayer);
  DoDisconnect(p, 0);
}

float DaggerHit(PartOne& p, const MpActor& aggressor, const MpActor& target,
                uint8_t difficulty)
{
  p.worldState.difficulty = difficulty;
  HitData hitData;
  hitData.aggressor = aggressor.GetFormId();
  hitData.target = target.GetFormId();
  hitData.source = kIronDagger;
  return TES5DamageFormula().CalculateDamage(aggressor, target, hitData);
}

float Health(const MpActor& actor)
{
  return actor.GetChangeForm().actorValues.healthPercentage;
}

// the share of its health the target loses to the caster's Firebolt, from
// full health
float FireboltLoss(PartOne& p, const MpActor& caster, MpActor& target,
                   uint8_t difficulty)
{
  p.worldState.difficulty = difficulty;
  ActorValues values = target.GetActorValues();
  values.healthPercentage = 1.f;
  target.SetPercentages(values);
  target.ApplyEffects(kFirebolt, MpActor::EffectsOf(&p.worldState, kFirebolt),
                      1.f, caster.GetFormId(), false);
  return 1.f - Health(target);
}
}

TEST_CASE("At Legendary, a bandit's weapon hit on a player costs three times "
          "its Adept damage in the server's record",
          "[Difficulty][espm]")
{
  PartOne& p = GetPartOne();
  auto& player = Player(p);
  auto& bandit = Bandit(p);

  const float adept = DaggerHit(p, bandit, player, kAdept);
  REQUIRE(adept > 0.f);
  REQUIRE(DaggerHit(p, bandit, player, kLegendary) ==
          Catch::Approx(adept * kLegendaryTaken));

  Leave(p);
}

TEST_CASE("At Legendary, a player's weapon hit on a bandit does a quarter of "
          "its Adept damage in the server's record",
          "[Difficulty][espm]")
{
  PartOne& p = GetPartOne();
  auto& player = Player(p);
  auto& bandit = Bandit(p);

  const float adept = DaggerHit(p, player, bandit, kAdept);
  REQUIRE(adept > 0.f);
  REQUIRE(DaggerHit(p, player, bandit, kLegendary) ==
          Catch::Approx(adept * kLegendaryDealt));

  Leave(p);
}

TEST_CASE("At Legendary, a bandit's Firebolt costs a player three times its "
          "Adept damage, and a player's costs a bandit a quarter",
          "[Difficulty][espm]")
{
  PartOne& p = GetPartOne();
  auto& player = Player(p);
  auto& bandit = Bandit(p);

  const float taken = FireboltLoss(p, bandit, player, kAdept);
  REQUIRE(taken > 0.f);
  REQUIRE(FireboltLoss(p, bandit, player, kLegendary) ==
          Catch::Approx(taken * kLegendaryTaken));

  const float dealt = FireboltLoss(p, player, bandit, kAdept);
  REQUIRE(dealt > 0.f);
  REQUIRE(FireboltLoss(p, player, bandit, kLegendary) ==
          Catch::Approx(dealt * kLegendaryDealt));

  Leave(p);
}

TEST_CASE("The player's own create word carries the server's difficulty, so "
          "its game's traps take the same one",
          "[Difficulty][espm]")
{
  PartOne& p = GetPartOne();
  p.worldState.difficulty = kLegendary;
  DoConnect(p, 0);
  p.CreateActor(kPlayer, { 0, 0, 0 }, 0, 0x3c);
  p.Messages().clear();
  p.SetUserActor(0, kPlayer);

  const auto own = std::find_if(
    p.Messages().begin(), p.Messages().end(), [&](auto& m) {
      return m.userId == 0 && m.j["t"] == MsgType::CreateActor &&
        m.j["isMe"] == true;
    });
  REQUIRE(own != p.Messages().end());
  REQUIRE(own->j["props"]["difficulty"] == kLegendary);

  Leave(p);
}
