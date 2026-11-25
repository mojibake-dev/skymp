#include "../skyrim-platform/src/platform_se/skyrim_platform/PuppetDetection.h"
#include <catch2/catch_all.hpp>
#include <vector>

// Whose answer the engine's detection reader gives on this game
// (PuppetDetection.h, the hook on Actor::RequestDetectionLevel): a puppet's
// detection of this game's player is the level its host relayed, so a
// guest's theft in full view of an NPC the host's game runs finds that NPC a
// witness in the guest's own StealAlarm. The header is std only, so the
// server's T0 build reaches it by path, as FigureNoiseTest.cpp reaches
// FigureNoise.h

TEST_CASE("A guest's theft in full view of an NPC the host's game runs is "
          "seen: the puppet's detection of this game's player answers the "
          "level its host relayed",
          "[PuppetDetection]")
{
  PuppetDetection::Relayed seen;
  seen.has = true;
  seen.level = 57;
  const auto answer = PuppetDetection::Answer(true, true, seen);
  REQUIRE(answer.has_value());
  REQUIRE(*answer == 57);
}

TEST_CASE("A guest's theft no NPC the host's game runs can see stays "
          "unseen: the host's relayed undetected level is the answer",
          "[PuppetDetection]")
{
  PuppetDetection::Relayed unseen;
  unseen.has = true;
  unseen.level = PuppetDetection::kUndetected;
  const auto answer = PuppetDetection::Answer(true, true, unseen);
  REQUIRE(answer.has_value());
  REQUIRE(*answer == PuppetDetection::kUndetected);
}

TEST_CASE("An NPC this game runs answers by its own AI, and a puppet's "
          "detection of another actor stays the engine's",
          "[PuppetDetection]")
{
  PuppetDetection::Relayed seen;
  seen.has = true;
  seen.level = 57;
  REQUIRE_FALSE(PuppetDetection::Answer(true, false, seen).has_value());
  REQUIRE_FALSE(PuppetDetection::Answer(false, true, seen).has_value());
}

TEST_CASE("A puppet whose host has relayed no level yet answers by the "
          "engine's own",
          "[PuppetDetection]")
{
  REQUIRE_FALSE(PuppetDetection::Answer(true, true, PuppetDetection::Relayed{})
                  .has_value());
}

TEST_CASE("The guest's stealth eye opens as the host's bandit finds it: the "
          "highest level against this game's player takes a puppet's "
          "relayed level above the engine's own",
          "[PuppetDetection]")
{
  const std::vector<std::int32_t> relayed = { PuppetDetection::kUndetected,
                                              57 };
  REQUIRE(PuppetDetection::HighestWithRelayed(PuppetDetection::kUndetected,
                                              relayed.data(),
                                              relayed.size()) == 57);
}

TEST_CASE("The guest's stealth eye closes as the host's bandit loses it: "
          "relayed undetected levels leave the engine's own",
          "[PuppetDetection]")
{
  const std::vector<std::int32_t> relayed = { PuppetDetection::kUndetected };
  REQUIRE(PuppetDetection::HighestWithRelayed(
            PuppetDetection::kUndetected, relayed.data(), relayed.size()) ==
          PuppetDetection::kUndetected);
  REQUIRE(PuppetDetection::HighestWithRelayed(PuppetDetection::kUndetected,
                                              nullptr, 0) ==
          PuppetDetection::kUndetected);
}

TEST_CASE("An NPC this game runs that sees the player keeps the eye at its "
          "own level when the puppets relay less",
          "[PuppetDetection]")
{
  const std::vector<std::int32_t> relayed = { PuppetDetection::kUndetected,
                                              10 };
  REQUIRE(PuppetDetection::HighestWithRelayed(40, relayed.data(),
                                              relayed.size()) == 40);
}
