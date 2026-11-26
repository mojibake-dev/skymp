#include "../skyrim-platform/src/platform_se/skyrim_platform/PartyLevelRule.h"
#include <catch2/catch_all.hpp>

// The party level rule (PartyLevelRule.h), the level the platform's detour of
// TESActorBaseData's GetLevel (id 14384) answers for a PC-level-mult base on
// a game told its party's level: the party's level times the base's mult as
// the engine computes its own player's (the integer product times 0.001f,
// cut), clamped to its calc levels when they are set. Ahtar 0x1325f's base
// (PC level mult 1000, calc levels 6 to 30) is the server's PartyLevelTest
// case.
namespace {
using PartyLevelRule::BaseFacts;
using PartyLevelRule::LevelFor;

BaseFacts Ahtar()
{
  BaseFacts b;
  b.pcLevelMult = true;
  b.level = 1000;
  b.calcLevelMin = 6;
  b.calcLevelMax = 30;
  return b;
}
}

TEST_CASE("A PC-level-mult base takes the party's level times its mult, "
          "clamped to its calc levels",
          "[PartyLevelRule]")
{
  REQUIRE(LevelFor(Ahtar(), 27) == std::optional<uint16_t>(27));
  REQUIRE(LevelFor(Ahtar(), 19) == std::optional<uint16_t>(19));
  // below his floor and above his ceiling
  REQUIRE(LevelFor(Ahtar(), 1) == std::optional<uint16_t>(6));
  REQUIRE(LevelFor(Ahtar(), 40) == std::optional<uint16_t>(30));

  // a mult of 1.2: 27 x 1200 = 32400, times 0.001, cut to 32
  BaseFacts tougher = Ahtar();
  tougher.level = 1200;
  REQUIRE(LevelFor(tougher, 27) == std::optional<uint16_t>(32));

  // no calc levels: no clamp, a half mult at party level 1 cut to 0 as the
  // engine cuts it
  BaseFacts unclamped = Ahtar();
  unclamped.level = 500;
  unclamped.calcLevelMin = 0;
  unclamped.calcLevelMax = 0;
  REQUIRE(LevelFor(unclamped, 1) == std::optional<uint16_t>(0));
  REQUIRE(LevelFor(unclamped, 60) == std::optional<uint16_t>(30));
}

TEST_CASE("The engine's own level stands with no party level told, without "
          "the PC level mult, and for the player's own base",
          "[PartyLevelRule]")
{
  REQUIRE(LevelFor(Ahtar(), 27).has_value());
  REQUIRE(!LevelFor(Ahtar(), 0).has_value());
  BaseFacts fixed = Ahtar();
  fixed.pcLevelMult = false;
  REQUIRE(!LevelFor(fixed, 27).has_value());
  BaseFacts player = Ahtar();
  player.playerBase = true;
  REQUIRE(!LevelFor(player, 27).has_value());
}
