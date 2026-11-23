#include "../skyrim-platform/src/platform_se/skyrim_platform/PushRoute.h"
#include <catch2/catch_all.hpp>

// Where a script's PushActorAway runs (PushRoute.h, the push-figure hook's
// rule): a player's body is its pusher's for the throw, so the pusher's own
// game throws its copy of the pushed player and every other game skips the
// push. The header is std only, so the server's T0 build reaches it by path,
// as HookPairingTest.cpp reaches HookPairing.h
namespace {
PushRoute::Route Route(bool pusherOwn, bool pusherFigure, bool targetOwn,
                       bool targetFigure, bool targetPuppet)
{
  PushRoute::Facts f;
  f.pusherIsOwnPlayer = pusherOwn;
  f.pusherIsPlayerFigure = pusherFigure;
  f.targetIsOwnPlayer = targetOwn;
  f.targetIsPlayerFigure = targetFigure;
  f.targetIsPuppet = targetPuppet;
  return PushRoute::Of(f);
}
}

TEST_CASE("This game's player's push onto another player's figure runs here: "
          "the pusher's game throws the pushed player's copy",
          "[PushRoute]")
{
  REQUIRE(Route(true, false, false, true, false) == PushRoute::Route::kRun);
}

TEST_CASE("Another player's figure's push onto this game's player is skipped: "
          "the pusher's game throws it, and this game follows that stream",
          "[PushRoute]")
{
  REQUIRE(Route(false, true, true, false, false) == PushRoute::Route::kSkip);
}

TEST_CASE("A bystander's game skips one figure's push onto another player's "
          "figure",
          "[PushRoute]")
{
  REQUIRE(Route(false, true, false, true, false) == PushRoute::Route::kSkip);
}

TEST_CASE("A push onto a puppet is skipped and reported, and everything else "
          "runs here as before",
          "[PushRoute]")
{
  REQUIRE(Route(true, false, false, false, true) ==
          PushRoute::Route::kSkipAndReport);
  REQUIRE(Route(false, true, false, false, true) ==
          PushRoute::Route::kSkipAndReport);
  // an NPC this game simulates, by its own player or by an NPC
  REQUIRE(Route(true, false, false, false, false) == PushRoute::Route::kRun);
  REQUIRE(Route(false, false, false, false, false) == PushRoute::Route::kRun);
  // this game's player pushed by an NPC, or by itself (a knock in place)
  REQUIRE(Route(false, false, true, false, false) == PushRoute::Route::kRun);
  REQUIRE(Route(true, false, true, false, false) == PushRoute::Route::kRun);
}
