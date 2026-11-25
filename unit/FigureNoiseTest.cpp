#include "../skyrim-platform/src/platform_se/skyrim_platform/FigureNoise.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <cstdint>
#include <initializer_list>
#include <vector>

// Whose noise an actor's detection hears as the player's (FigureNoise.h, the
// hearing hooks on CalculateDetection): another player's figure is that
// player on this game, so an arrow it lands beside an actor this game runs
// is heard as the player's arrow is. The header is std only, so the
// server's T0 build reaches it by path, as PushRouteTest.cpp reaches
// PushRoute.h
namespace {
// three actors' addresses on a game: its own player, another player's
// figure, an NPC
constexpr std::uintptr_t kPlayer = 0x1000;
constexpr std::uintptr_t kFigure = 0x2000;
constexpr std::uintptr_t kNpc = 0x3000;

FigureNoise::Target Player()
{
  FigureNoise::Target t;
  t.isEnginePlayer = true;
  return t;
}

FigureNoise::Target Figure()
{
  FigureNoise::Target t;
  t.isPlayerFigure = true;
  return t;
}

FigureNoise::Target Npc()
{
  return FigureNoise::Target{};
}
}

TEST_CASE("This game's own player's noise is read out of combat and carries "
          "the player's distance, as the engine has it",
          "[FigureNoise]")
{
  REQUIRE(FigureNoise::CountsAsPlayer(Player()));
  REQUIRE(FigureNoise::ComparedWith(Player(), kPlayer, kPlayer) == kPlayer);
}

TEST_CASE("An NPC's noise stays unread out of combat and carries an NPC's "
          "distance, as the engine has it",
          "[FigureNoise]")
{
  REQUIRE_FALSE(FigureNoise::CountsAsPlayer(Npc()));
  REQUIRE(FigureNoise::ComparedWith(Npc(), kNpc, kPlayer) == kPlayer);
}

TEST_CASE("A guest's arrow landing beside an NPC this game runs is heard: "
          "another player's figure's noise is read out of combat as this "
          "game's own player's is",
          "[FigureNoise]")
{
  REQUIRE(FigureNoise::CountsAsPlayer(Figure()));
}

TEST_CASE("A guest's noise carries the player's distance, and its arrows in "
          "flight are watched at the player's distance",
          "[FigureNoise]")
{
  REQUIRE(FigureNoise::ComparedWith(Figure(), kFigure, kPlayer) == kFigure);
}

namespace {
// CalculateDetection's code as the hooks read it: its three sites' bytes at
// their offsets, every other byte zero
std::vector<std::uint8_t> Code()
{
  std::vector<std::uint8_t> code(FigureNoise::kCodeSize, 0);
  std::copy(FigureNoise::kGateBytes.begin(), FigureNoise::kGateBytes.end(),
            code.begin() + FigureNoise::kGate);
  std::copy(FigureNoise::kNoiseDistanceBytes.begin(),
            FigureNoise::kNoiseDistanceBytes.end(),
            code.begin() + FigureNoise::kNoiseDistance);
  std::copy(FigureNoise::kProjectileWatchBytes.begin(),
            FigureNoise::kProjectileWatchBytes.end(),
            code.begin() + FigureNoise::kProjectileWatch);
  std::copy(FigureNoise::kCombatSightBytes.begin(),
            FigureNoise::kCombatSightBytes.end(),
            code.begin() + FigureNoise::kCombatSight);
  return code;
}
}

TEST_CASE("The hooks find the player's global and GetHostileToActor in "
          "CalculateDetection's code, wherever the game is loaded",
          "[FigureNoise]")
{
  const auto code = Code();
  // 1.6.1170 at its preferred base: 37774 at 0x690870, the player's global
  // (401069) at 0x3137698, GetHostileToActor (37537) at 0x679f10
  const auto found = FigureNoise::Find(code.data(), 0x140690870);
  REQUIRE(found.has_value());
  REQUIRE(found->playerSlot == 0x143137698);
  REQUIRE(found->hostileCall == 0x140679f10);
  // the same image at another base
  const auto moved = FigureNoise::Find(code.data(), 0x7ff700690870);
  REQUIRE(moved.has_value());
  REQUIRE(moved->playerSlot == 0x7ff703137698);
  REQUIRE(moved->hostileCall == 0x7ff700679f10);
}

TEST_CASE("CalculateDetection is left unwritten when any of its three sites "
          "holds other bytes",
          "[FigureNoise]")
{
  for (const std::size_t at :
       { FigureNoise::kGate + 8, FigureNoise::kGateCall + 2,
         FigureNoise::kNoiseDistance + 4,
         FigureNoise::kProjectileWatch + 11 }) {
    auto code = Code();
    code[at] ^= 0xff;
    REQUIRE_FALSE(FigureNoise::Find(code.data(), 0x140690870).has_value());
  }
}

TEST_CASE("An NPC fighting someone else sees a guest at the player's whole "
          "sight distance: the hooks find CalculateDetection's in-combat "
          "sight test and where its jump past the share lands",
          "[FigureNoise]")
{
  // 1.6.1170 at its preferred base: the jump at +0x171 lands at +0x1ae
  const auto found = FigureNoise::Find(Code().data(), 0x140690870);
  REQUIRE(found.has_value());
  REQUIRE(found->combatSightWhole == 0x140690a1e);
  const auto moved = FigureNoise::Find(Code().data(), 0x7ff700690870);
  REQUIRE(moved.has_value());
  REQUIRE(moved->combatSightWhole == 0x7ff700690a1e);
}

TEST_CASE("CalculateDetection is left unwritten when its in-combat sight "
          "test holds other bytes",
          "[FigureNoise]")
{
  for (const std::size_t at :
       { FigureNoise::kCombatSight + 1, FigureNoise::kCombatSight + 4,
         FigureNoise::kCombatSight + 8 }) {
    auto code = Code();
    code[at] ^= 0xff;
    REQUIRE_FALSE(FigureNoise::Find(code.data(), 0x140690870).has_value());
  }
}
