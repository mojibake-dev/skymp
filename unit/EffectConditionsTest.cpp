#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActorValueRecord.h"
#include "Condition.h"
#include "ConditionsEvaluator.h"
#include "Appearance.h"
#include "MpActor.h"
#include "RunningEffect.h"
#include "libespm/espm.h"
#include <chrono>

PartOne& GetPartOne();

// thuum docs/DECISIONS.md ADR-039: the server evaluates an effect's
// conditions before applying it to another player; a function it cannot
// evaluate refuses the effect. Firebolt (Skyrim.esm 0x12FD0) carries the
// Impact perk's stagger rider, gated by HasPerk on the effect record
// (unit/PerkRiderTest.cpp), and its fire damage with no conditions.
namespace {
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;

std::pair<MpActor*, MpActor*> Pair(PartOne& p)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kCaster, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kCaster);
  p.CreateActor(kTarget, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  return { &p.worldState.GetFormAt<MpActor>(kCaster),
           &p.worldState.GetFormAt<MpActor>(kTarget) };
}

void Leave(PartOne& p)
{
  p.DestroyActor(kCaster);
  p.DestroyActor(kTarget);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}

Condition Row(const char* function, uint16_t index, const char* runsOn,
              const char* comparison, float value, uint32_t parameter1,
              const char* logical)
{
  Condition c;
  c.function = function;
  c.functionIndex = index;
  c.runsOn = runsOn;
  c.comparison = comparison;
  c.value = value;
  c.parameter1 = fmt::format("0x{:X}", parameter1);
  c.parameter2 = "0x0";
  c.logicalOperator = logical;
  return c;
}

bool Holds(PartOne& p, const std::vector<Condition>& conditions,
           const MpActor& victim, const MpActor& caster)
{
  bool holds = false;
  ConditionEvaluatorContext context;
  ConditionsEvaluator::EvaluateConditions(
    p.worldState.conditionFunctionMap,
    p.worldState.conditionsEvaluatorSettings,
    ConditionsEvaluatorCaller::kMagicEffect, conditions, victim, caster,
    [&](bool result, std::vector<std::string>&) { holds = result; }, context);
  return holds;
}
}

TEST_CASE("Firebolt's perk rider is refused on another player while its fire "
          "damage holds",
          "[Conditions][espm]")
{
  PartOne& p = GetPartOne();
  auto [caster, target] = Pair(p);
  const auto effects = MpActor::EffectsOf(&p.worldState, kFirebolt);
  REQUIRE_FALSE(effects.empty());
  int riders = 0;
  int damages = 0;
  for (const auto& effect : effects) {
    const auto mgef =
      espm::GetData<espm::MGEF>(effect.effectId, &p.worldState);
    if (mgef.callsHasPerk) {
      ++riders;
      // the record keeps its conditions now, and HasPerk is unknown to the
      // server before M5: refused, not applied
      REQUIRE_FALSE(mgef.conditions.empty());
      REQUIRE_FALSE(MpActor::EffectConditionsHold(
        &p.worldState, effect, kFirebolt, *target, *caster));
    } else if (mgef.data.effectType == espm::MGEF::EffectType::ValueMod &&
               mgef.data.primaryAV == espm::ActorValue::Health) {
      ++damages;
      REQUIRE(mgef.conditions.empty());
      REQUIRE(MpActor::EffectConditionsHold(&p.worldState, effect, kFirebolt,
                                            *target, *caster));
    }
  }
  REQUIRE(riders >= 1);
  REQUIRE(damages >= 1);
  Leave(p);
}

TEST_CASE("A summon-only effect refuses a player, and the functions the "
          "server knows answer from its records",
          "[Conditions]")
{
  PartOne& p = GetPartOne();
  auto [caster, target] = Pair(p);

  // Azure Reconstruction's shape (apocrypha's read, 2026-10-18): the
  // target a commanded actor, or (OR row) its Health below full
  const std::vector<Condition> azure = {
    Row("IsCommandedActor", 700, "Subject", "==", 1.f, 0, "AND"),
    Row("GetActorValuePercent", 640, "Subject", "<", 1.f, 0x3E8, "OR"),
  };
  REQUIRE_FALSE(Holds(p, azure, *target, *caster));

  // an actor is an actor; a living one is not dead
  REQUIRE(Holds(p, { Row("IsActor", 353, "Subject", "==", 1.f, 0, "AND") },
                *target, *caster));
  REQUIRE(Holds(p, { Row("GetDead", 46, "Subject", "==", 0.f, 0, "AND") },
                *target, *caster));

  // a keyword the actor's race carries, read from the records themselves
  auto& br = p.worldState.GetEspm().GetBrowser();
  const auto race = br.LookupById(target->GetRaceId());
  REQUIRE(race.rec != nullptr);
  const auto keywords = race.rec->GetKeywordIds(p.worldState.GetEspmCache());
  REQUIRE_FALSE(keywords.empty());
  const uint32_t carried = race.ToGlobalId(keywords.front());
  REQUIRE(
    Holds(p, { Row("HasKeyword", 560, "Subject", "==", 1.f, carried, "AND") },
          *target, *caster));
  REQUIRE_FALSE(Holds(
    p, { Row("HasKeyword", 560, "Subject", "==", 1.f, 0x00FFFFFF, "AND") },
    *target, *caster));
  Leave(p);
}

TEST_CASE("IsEssential and EffectWasDualCast answer: a player is never "
          "essential, and the dual flag is the claimed cast's",
          "[Conditions]")
{
  // thuum train-29 (docs/verbs/spell-cast.md). Finger of Death's one effect
  // gates on IsEssential == 0 (Apocalypse's census row 86); playtest
  // twenty-five's server refused it as unknown (354). A player is never
  // essential, so the row holds on another player.
  PartOne& p = GetPartOne();
  auto [caster, target] = Pair(p);
  REQUIRE(Holds(p, { Row("IsEssential", 354, "Subject", "==", 0.f, 0, "AND") },
                *target, *caster));
  REQUIRE_FALSE(
    Holds(p, { Row("IsEssential", 354, "Subject", "==", 1.f, 0, "AND") },
          *target, *caster));

  // EffectWasDualCast (724) reads the context the hit's claim fills from
  // the cast record (SpellCastMessage.isDualCasting); a caller that knows
  // nothing of the cast leaves it false, so a 724 rider never holds there
  const std::vector<Condition> dualRider = { Row(
    "EffectWasDualCast", 724, "Subject", "==", 1.f, 0, "AND") };
  REQUIRE_FALSE(Holds(p, dualRider, *target, *caster));
  bool holds = false;
  ConditionEvaluatorContext dual;
  dual.effectWasDualCast = true;
  ConditionsEvaluator::EvaluateConditions(
    p.worldState.conditionFunctionMap,
    p.worldState.conditionsEvaluatorSettings,
    ConditionsEvaluatorCaller::kMagicEffect, dualRider, *target, *caster,
    [&](bool result, std::vector<std::string>&) { holds = result; }, dual);
  REQUIRE(holds);
  Leave(p);
}

TEST_CASE("GetIsID and GetGlobalValue answer from the records: Dragonborn's "
          "Unrelenting Force gate",
          "[EffectConditions][espm]")
{
  // Dragonborn.esm overrides VoiceUnrelentingForce1 and 3 with an extra
  // damage effect gated by GetIsID(Player 0x7) == 1 on Target (the caster
  // is a player's actor) AND GetGlobalValue(DLC2BlackBookReward2) == 1, the
  // "Dragonborn Force" Black Book reward (lab/esm.py on sky-srv, 2026-10-19;
  // the nightly's b-push read 10 damage, the vanilla number, once the
  // server stopped applying the gated 40 unconditionally)
  PartOne& p = GetPartOne();
  auto [caster, target] = Pair(p);
  constexpr uint32_t kPlayerBase = 0x00000007;
  // Dragonborn.esm is the fifth master here (unit/main.cpp): 0x04 prefix
  constexpr uint32_t kBlackBookReward2 = 0x04020e98;
  REQUIRE(
    Holds(p, { Row("GetIsID", 72, "Target", "==", 1.f, kPlayerBase, "AND") },
          *target, *caster));
  REQUIRE_FALSE(
    Holds(p, { Row("GetIsID", 72, "Target", "==", 1.f, 0x00013bb8, "AND") },
          *target, *caster));
  // the records' default is 0: the reward is not earned
  REQUIRE_FALSE(Holds(p,
                      { Row("GetGlobalValue", 74, "Subject", "==", 1.f,
                            kBlackBookReward2, "AND") },
                      *target, *caster));
  REQUIRE(Holds(p,
                { Row("GetGlobalValue", 74, "Subject", "==", 0.f,
                      kBlackBookReward2, "AND") },
                *target, *caster));
  // the player's own recorded global wins over the records
  REQUIRE(target->SetPlayerGlobals({ PlayerGlobal{
    FormDesc::FromFormId(kBlackBookReward2, p.worldState.espmFiles), 1.f } }));
  REQUIRE(Holds(p,
                { Row("GetGlobalValue", 74, "Subject", "==", 1.f,
                      kBlackBookReward2, "AND") },
                *target, *caster));
  Leave(p);
}

// thuum docs/verbs/spell-cast.md (train-30, Hailstone): GetActorValue (14)
// answers the current Health, Magicka or Stamina as the maximum times the
// kept percentage, and any other actor value as the player's recorded base
// plus the modifier of the effects the server applied (0 with no record);
// HasMagicEffect (214) answers from the running effects, by the MGEF's id.
TEST_CASE("GetActorValue and HasMagicEffect answer from what the server "
          "holds: the current Health, a recorded base, a running effect",
          "[Conditions]")
{
  PartOne& p = GetPartOne();
  auto [caster, target] = Pair(p);
  // Health (24, CommonLibSSE-NG include/RE/A/ActorValues.h): 100 at full
  REQUIRE(Holds(p, { Row("GetActorValue", 14, "Subject", ">", 50.f, 24, "AND") },
                *target, *caster));
  REQUIRE_FALSE(
    Holds(p, { Row("GetActorValue", 14, "Subject", "==", 0.f, 24, "AND") },
          *target, *caster));
  // ResistFrost (43) with no record: 0
  REQUIRE(Holds(p, { Row("GetActorValue", 14, "Subject", "==", 0.f, 43, "AND") },
                *target, *caster));
  REQUIRE_FALSE(
    Holds(p, { Row("GetActorValue", 14, "Subject", ">", 0.f, 43, "AND") },
          *target, *caster));
  // the player's recorded base (the client's ActorValues report)
  ActorValueRecord record;
  record.bases = { { 43, 50.f } };
  target->SetActorValueRecord(record);
  REQUIRE(Holds(p, { Row("GetActorValue", 14, "Subject", ">", 0.f, 43, "AND") },
                *target, *caster));
  REQUIRE(Holds(p, { Row("GetActorValue", 14, "Subject", "==", 50.f, 43, "AND") },
                *target, *caster));
  // HasMagicEffect: nothing running
  REQUIRE(Holds(p,
                { Row("HasMagicEffect", 214, "Subject", "==", 0.f, 0x3EAF3,
                      "AND") },
                *target, *caster));
  // Fortify Health (Skyrim.esm MGEF 0x3EAF3 of 0x3EAF2) running for 60 s
  const auto nowMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count());
  RunningEffect fortify;
  fortify.effect = FormDesc::FromString("3eaf3:Skyrim.esm");
  fortify.source = FormDesc::FromString("3eaf2:Skyrim.esm");
  fortify.kind = 1;
  fortify.av = 24;
  fortify.magnitude = 20.f;
  fortify.durationS = 60.f;
  fortify.elapsedS = 0.f;
  fortify.asOfMs = nowMs;
  MpChangeForm changeForm = target->GetChangeForm();
  changeForm.runningEffects = std::vector<RunningEffect>{ fortify };
  target->ApplyChangeForm(changeForm);
  REQUIRE(Holds(p,
                { Row("HasMagicEffect", 214, "Subject", "==", 1.f, 0x3EAF3,
                      "AND") },
                *target, *caster));
  // the spell's id is not an effect's
  REQUIRE(Holds(p,
                { Row("HasMagicEffect", 214, "Subject", "==", 0.f, 0x3EAF2,
                      "AND") },
                *target, *caster));
  // and the current Health reads the raised maximum, 120 at full
  REQUIRE(Holds(p,
                { Row("GetActorValue", 14, "Subject", ">", 110.f, 24, "AND") },
                *target, *caster));
  Leave(p);
}

// thuum docs/verbs/spell-cast.md (train-30, Hailstone): a race's constant
// abilities stand in the server's modifier from the records (RACE SPLO),
// so a Nord reads its 50 ResistFrost (Skyrim.esm 000AA020) with no record
// and no running effect, and an Altmer's maximum Magicka carries Highborn's
// 50 (UESP Skyrim:Nord, Skyrim:High Elf).
TEST_CASE("A race's constant abilities stand in GetActorValue and the "
          "maxima: a Nord's frost resistance, an Altmer's magicka",
          "[Conditions]")
{
  PartOne& p = GetPartOne();
  auto [caster, target] = Pair(p);
  Appearance nord;
  nord.raceId = 0x00013746; // Skyrim.esm NordRace
  target->SetAppearance(&nord);
  REQUIRE(Holds(p, { Row("GetActorValue", 14, "Subject", "==", 50.f, 43, "AND") },
                *target, *caster));
  REQUIRE(Holds(p, { Row("GetActorValue", 14, "Subject", ">", 0.f, 43, "AND") },
                *target, *caster));
  REQUIRE(Holds(p, { Row("GetActorValue", 14, "Subject", "==", 0.f, 42, "AND") },
                *target, *caster)); // ResistFire: none for a Nord
  Appearance altmer;
  altmer.raceId = 0x00013743; // Skyrim.esm HighElfRace
  target->SetAppearance(&altmer);
  REQUIRE(Holds(p, { Row("GetActorValue", 14, "Subject", "==", 0.f, 43, "AND") },
                *target, *caster));
  REQUIRE(target->GetMaximumValues().magicka ==
          Catch::Approx(target->GetBaseValues().magicka + 50.f));
  Leave(p);
}
