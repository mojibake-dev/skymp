#include "GetBaseActorValues.h"
#include "EvaluateTemplate.h"
#include "WorldState.h"
#include "libespm/CLAS.h"
#include "libespm/GMST.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <spdlog/spdlog.h>

void BaseActorValues::VisitBaseActorValuesAndPercentages(
  BaseActorValues& baseActorValues, MpChangeForm& changeForm,
  CreateActorMessage& message)
{
  message.props.health = baseActorValues.health;
  message.props.stamina = baseActorValues.stamina;
  message.props.magicka = baseActorValues.magicka;
  message.props.healRate = baseActorValues.healRate;
  message.props.staminaRate = baseActorValues.staminaRate;
  message.props.magickaRate = baseActorValues.magickaRate;
  message.props.healRateMult = baseActorValues.healRateMult;
  message.props.staminaRateMult = baseActorValues.staminaRateMult;
  message.props.magickaRateMult = baseActorValues.magickaRateMult;
  message.props.healthPercentage = changeForm.actorValues.healthPercentage;
  message.props.staminaPercentage = changeForm.actorValues.staminaPercentage;
  message.props.magickaPercentage = changeForm.actorValues.magickaPercentage;
}

namespace {
// a setting the game files hold, or `fallback` where none is read
float SettingOf(WorldState* worldState, uint32_t id, float fallback)
{
  try {
    return espm::GetData<espm::GMST>(id, worldState).value;
  } catch (...) {
    return fallback;
  }
}

int32_t IntSettingOf(WorldState* worldState, uint32_t id, int32_t fallback)
{
  try {
    return espm::GetData<espm::GMST>(id, worldState).intValue;
  } catch (...) {
    return fallback;
  }
}

// An auto-calc NPC's level as the engine reads it (TESActorBaseData's level,
// id 14384): its ACBS level, or with the PC level mult the player level
// times the mult, clamped to its calc levels; the player level is the party
// level the engine running it is told
int32_t AutoCalcLevel(const espm::NPC_::Data& npc, uint16_t playerLevel)
{
  int32_t level = npc.level;
  if (npc.pcLevelMult) {
    level = static_cast<int32_t>(std::max<uint16_t>(playerLevel, 1) *
                                 npc.level * 0.001);
    if (npc.calcLevelMin) {
      level = std::max(level, static_cast<int32_t>(npc.calcLevelMin));
    }
    if (npc.calcLevelMax) {
      level = std::min(level, static_cast<int32_t>(npc.calcLevelMax));
    }
  }
  return std::max(level, 1);
}

// the level's points shared out by the class's weights (the engine's 24899):
// (level - 1) * per-level points P over the weights' sum W, each attribute
// taking its weight times floor(P / W), then the remainder one point at a
// time in the order of descending weight, the later attribute first on a tie
// (stamina, magicka, health), each at most its weight in remainder points.
// Indices: 0 health, 1 magicka, 2 stamina
std::array<int32_t, 3> LevelShare(int32_t levelsPast, int32_t perLevel,
                                  const std::array<int32_t, 3>& weights)
{
  std::array<int32_t, 3> share{ 0, 0, 0 };
  const int32_t total = weights[0] + weights[1] + weights[2];
  const int32_t points = std::max(levelsPast, 0) * std::max(perLevel, 0);
  if (total <= 0 || points <= 0) {
    return share;
  }
  const int32_t each = points / total;
  for (size_t i = 0; i < 3; ++i) {
    share[i] = weights[i] * each;
  }
  int32_t left = points - each * total;
  std::array<size_t, 3> order{ 2, 1, 0 };
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return weights[a] > weights[b];
  });
  std::array<int32_t, 3> given{ 0, 0, 0 };
  while (left > 0) {
    bool gave = false;
    for (size_t i : order) {
      if (left > 0 && given[i] < weights[i]) {
        ++given[i];
        ++share[i];
        --left;
        gave = true;
      }
    }
    if (!gave) {
      break;
    }
  }
  return share;
}
}

BaseActorValues GetBaseActorValues(WorldState* worldState, uint32_t baseId,
                                   uint32_t raceIdOverride,
                                   const std::vector<FormDesc>& templateChain,
                                   uint16_t playerLevel)
{

  auto npcData = espm::GetData<espm::NPC_>(baseId, worldState);

  uint32_t raceId = raceIdOverride
    ? raceIdOverride
    : EvaluateTemplate<espm::NPC_::UseTraits>(
        worldState, baseId, templateChain,
        [](const auto& npcLookupResult, const auto& npcData) {
          return npcLookupResult.ToGlobalId(npcData.race);
        });
  auto raceData = espm::GetData<espm::RACE>(raceId, worldState);

  espm::NPC_::Data attributesNpcData = EvaluateTemplate<espm::NPC_::UseStats>(
    worldState, baseId, templateChain,
    [](const auto&, const auto& npcData) { return npcData; });

  BaseActorValues actorValues;

  // an auto-calc NPC's values as the engine computes them (its autocalc,
  // id 24765, read back by the actor value owner at 24774): the race's
  // start and the offset, the health's level bonus, and the class's share of
  // the level's points; any other NPC's are the race's start and the offset
  int32_t healthBonus = 0;
  std::array<int32_t, 3> share{ 0, 0, 0 };
  if (attributesNpcData.autoCalcStats) {
    const uint32_t classId = EvaluateTemplate<espm::NPC_::UseStats>(
      worldState, baseId, templateChain,
      [](const auto& npcLookupResult, const auto& npcData) {
        return npcData.classId ? npcLookupResult.ToGlobalId(npcData.classId)
                               : 0u;
      });
    const auto classLookup = classId
      ? worldState->GetEspm().GetBrowser().LookupById(classId)
      : espm::LookupResult{};
    const auto* clas = espm::Convert<espm::CLAS>(classLookup.rec);
    if (clas && raceId) {
      const auto classData = clas->GetData(worldState->GetEspmCache());
      const int32_t levelsPast =
        AutoCalcLevel(attributesNpcData, playerLevel) - 1;
      const float bonus = baseId == 0x7
        ? SettingOf(worldState, espm::GMST::kFPCHealthLevelBonus, 0.f)
        : SettingOf(worldState, espm::GMST::kFNPCHealthLevelBonus, 5.f);
      healthBonus = static_cast<int32_t>(levelsPast * bonus);
      share = LevelShare(
        levelsPast, IntSettingOf(worldState, espm::GMST::kIAVDhmsLevelUp, 10),
        { classData.healthWeight, classData.magickaWeight,
          classData.staminaWeight });
    }
  }

  actorValues.health = static_cast<int32_t>(raceData.startingHealth) +
    attributesNpcData.healthOffset + healthBonus + share[0];
  if (actorValues.health <= 0) {
    spdlog::warn("GetBaseActorValues {:x} {:x} - Negative Health found: "
                 "startingHealth={}, healthOffset={}, defaulting to 100",
                 baseId, raceIdOverride, raceData.startingHealth,
                 attributesNpcData.healthOffset);
    actorValues.health = 100.f;
  }

  actorValues.magicka = static_cast<int32_t>(raceData.startingMagicka) +
    attributesNpcData.magickaOffset + share[1];
  if (actorValues.magicka < 0) { // zero magicka is ok, negative isn't
    spdlog::warn("GetBaseActorValues {:x} {:x} - Negative Magicka found: "
                 "startingMagicka={}, magickaOffset={}, defaulting to 100",
                 baseId, raceIdOverride, raceData.startingMagicka,
                 attributesNpcData.magickaOffset);
    actorValues.magicka = 100.f;
  }

  actorValues.stamina = static_cast<int32_t>(raceData.startingStamina) +
    attributesNpcData.staminaOffset + share[2];
  if (actorValues.stamina <= 0) {
    spdlog::warn("GetBaseActorValues {:x} {:x} - Negative Stamina found: "
                 "startingStamina={}, staminaOffset={}, defaulting to 100",
                 baseId, raceIdOverride, raceData.startingStamina,
                 attributesNpcData.staminaOffset);
    actorValues.stamina = 100.f;
  }

  actorValues.healRate = raceData.healRegen;
  actorValues.magickaRate = raceData.magickaRegen;
  actorValues.staminaRate = raceData.staminaRegen;

  spdlog::trace(
    "GetBaseActorValues {:x} {:x} - startingHealth={}, healthOffset={}",
    baseId, raceIdOverride, raceData.startingHealth,
    attributesNpcData.healthOffset);

  return actorValues;
}
