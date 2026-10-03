#include "MovementBudget.h"
#include <catch2/catch_all.hpp>

using namespace std::chrono_literals;

// thuum docs/verbs/movement-speed.md
TEST_CASE("The movement budget holds a burst and refills at the top speed",
          "[MovementValidation]")
{
  MovementBudget budget;
  const auto t0 = MovementBudget::Clock::now();
  REQUIRE(budget.Spend(MovementBudget::kBurst, t0));
  REQUIRE(!budget.Spend(1.f, t0));
  // a refused move spends nothing; a second later a second's worth is back
  REQUIRE(budget.Spend(MovementBudget::kMaxSpeed, t0 + 1s));
  REQUIRE(!budget.Spend(1.f, t0 + 1s));
  // an idle minute refills no more than the burst
  REQUIRE(budget.Spend(MovementBudget::kBurst, t0 + 61s));
  REQUIRE(!budget.Spend(1.f, t0 + 61s));
}

TEST_CASE("A horse's sprint fits the movement budget, a speed hack does not",
          "[MovementValidation]")
{
  // one update every 130 ms (skymp5-client sendInputsService.ts)
  MovementBudget rider, hacker;
  auto t = MovementBudget::Clock::now();
  bool riderFits = true;
  int hackerRefusedAt = -1;
  for (int i = 0; i < 600; ++i) {
    t += 130ms;
    riderFits = riderFits && rider.Spend(600.f * 0.13f, t);
    if (!hacker.Spend(1500.f * 0.13f, t) && hackerRefusedAt < 0) {
      hackerRefusedAt = i;
    }
  }
  REQUIRE(riderFits);
  REQUIRE(hackerRefusedAt >= 0);
  REQUIRE(hackerRefusedAt < 20);
}
