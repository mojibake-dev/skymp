#pragma once
#include <algorithm>
#include <chrono>
#include <optional>

// thuum docs/verbs/movement-speed.md. How far a player's actor may move over
// the ground: a budget that refills at the fastest speed a player's movement
// types allow and holds a burst's worth, so a dash, a knockback or a burst of
// updates after a stall passes while a sustained speed above the game's does
// not. Height is free: falls are the engine's.
class MovementBudget
{
public:
  using Clock = std::chrono::steady_clock;

  // Horse and Vampire Lord sprint, 600 units a second, the fastest movement
  // type a player uses (Skyrim.esm Horse_Sprint_MT, Dawnguard.esm
  // VampireLordSprint_MT), plus a tenth
  static constexpr float kMaxSpeed = 660.f;
  static constexpr float kBurst = 2048.f;

  // True when a move of `distance` over the ground fits the budget at `now`;
  // only then is it spent.
  bool Spend(float distance, Clock::time_point now)
  {
    if (last) {
      const float seconds = std::chrono::duration<float>(now - *last).count();
      budget = std::min(kBurst, budget + kMaxSpeed * std::max(seconds, 0.f));
    }
    last = now;
    if (distance > budget) {
      return false;
    }
    budget -= distance;
    return true;
  }

private:
  float budget = kBurst;
  std::optional<Clock::time_point> last;
};
