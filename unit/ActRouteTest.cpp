#include "../skyrim-platform/src/platform_se/skyrim_platform/ActRoute.h"
#include <catch2/catch_all.hpp>

// Where an act on the player runs (ActRoute.h, OnStaged's rule): a native
// the scoped column runs on the acting player's seat alone, called by a
// script this game runs for another player, never runs here. From a press or
// an entry this game runs for its presser it is forwarded to the acting
// player's own game; from a stage or a word another game set it is that
// game's own already. The header is std only, so the server's T0 build
// reaches it by path, as PushRouteTest.cpp reaches PushRoute.h
namespace {
ActRoute::Route Route(bool actingSeatAlone, bool otherPlayerActs,
                      bool pressRoot)
{
  ActRoute::Facts f;
  f.actingSeatAlone = actingSeatAlone;
  f.otherPlayerActs = otherPlayerActs;
  f.pressRoot = pressRoot;
  return ActRoute::Of(f);
}

constexpr uint32_t kFigure = 0xff000d2f;
constexpr uint32_t kLearnWordFade = 0x4e77b;
}

TEST_CASE("The host's run of a wall the guest entered forwards the wall's "
          "teach to the guest's own game and never teaches the host's player",
          "[ActRoute]")
{
  REQUIRE(Route(true, true, true) == ActRoute::Route::kForward);
}

TEST_CASE("A stage the acting player's own game set and relayed here skips "
          "its act: that game made it already",
          "[ActRoute]")
{
  REQUIRE(Route(true, true, false) == ActRoute::Route::kSkip);
}

TEST_CASE("Outside another player's acting scope an act runs on this game's "
          "own player, and a row run anywhere runs here",
          "[ActRoute]")
{
  REQUIRE(Route(true, false, false) == ActRoute::Route::kRun);
  REQUIRE(Route(true, false, true) == ActRoute::Route::kRun);
  REQUIRE(Route(false, true, true) == ActRoute::Route::kRun);
  REQUIRE(Route(false, true, false) == ActRoute::Route::kRun);
}

TEST_CASE("A forwarded act names the player for a static or the acting "
          "player's figure, and a form as itself",
          "[ActRoute]")
{
  // Game.TeachWord, a static on no object: no self of its own
  REQUIRE(ActRoute::SelfOf(true, 0, kFigure) == 0x14);
  // Actor.SetPlayerResistingArrest on the player, swapped to the figure here
  REQUIRE(ActRoute::SelfOf(false, kFigure, kFigure) == 0x14);
  // ImageSpaceModifier.Apply on the wall's fade
  REQUIRE(ActRoute::SelfOf(false, kLearnWordFade, kFigure) == kLearnWordFade);
  // a scope with no figure here names nothing as the figure
  REQUIRE(ActRoute::SelfOf(false, 0, 0) == 0);
}
