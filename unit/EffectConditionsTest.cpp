#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActorValueRecord.h"
#include "Condition.h"
#include "ConditionsEvaluator.h"
#include "MpActor.h"
#include "libespm/espm.h"

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
