#pragma once
#include "papyrus-vm/CIString.h"
#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>

class MpActor;
class WorldState;
struct AnimationData;

class AnimationSystem
{
public:
  AnimationSystem();
  void Init(WorldState* worldState);
  void Process(MpActor* actor, const AnimationData& animData);
  void ClearInfo(MpActor* actor);

  // When the actor last started a power attack (any attackPowerStart event
  // its client sent), the epoch if never: thuum docs/verbs/damage-flags.md
  std::chrono::steady_clock::time_point GetLastPowerAttackStartTime(
    const MpActor& actor) const;
  // When the actor last started a bash (bashStart), the epoch if never:
  // thuum docs/verbs/blocking.md
  std::chrono::steady_clock::time_point GetLastBashStartTime(
    const MpActor& actor) const;
  // The last attack the actor started (an attackStart, attackPowerStart,
  // bashStart or bashPowerStart event, the name its race's ATKE gives it,
  // in lower case), empty if none: its stagger offset is that attack's
  // (thuum docs/verbs/stagger.md)
  std::string GetLastAttackStartName(const MpActor& actor) const;
  void SetWeaponStaminaModifiers(
    std::unordered_map<std::string, float>&& modifiers);

private:
  using AnimationCallback = std::function<void(MpActor*)>;
  using AnimationCallbacks = CIMap<AnimationCallback>;
  using AnimationTimePoints =
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>;

  void InitAdditionalCallbacks();
  std::chrono::steady_clock::time_point GetLastAttackReleaseAnimationTime(
    MpActor* actor) const;
  void SetLastAttackReleaseAnimationTime(
    MpActor* actor,
    std::chrono::steady_clock::time_point timePoint =
      std::chrono::steady_clock::now());
  std::vector<uint32_t> GetWeaponKeywordFormIds(uint32_t baseId) const;
  std::vector<std::string_view> GetWeaponKeywords(uint32_t baseId) const;
  float ComputeWeaponStaminaModifier(uint32_t baseId) const;
  void HandleAttackAnim(MpActor* actor, float defaultModifier) const;
  bool IsWorldStateAttached() const noexcept;

private:
  std::unordered_map<std::string, float> weaponStaminaModifiers;

  AnimationCallbacks animationCallbacks;
  AnimationTimePoints lastAttackReleaseAnimationTimePoints;
  AnimationTimePoints lastPowerAttackStartTimePoints;
  AnimationTimePoints lastBashStartTimePoints;
  std::unordered_map<uint32_t, std::string> lastAttackStartNames;
  WorldState* worldState = nullptr;
  bool hasSweetpie = false;
};
