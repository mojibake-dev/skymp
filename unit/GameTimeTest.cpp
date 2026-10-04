#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "SetGameTimeMessage.h"
#include "script_classes/PapyrusUtility.h"
#include "script_compatibility_policies/HeuristicPolicy.h"

using Catch::Matchers::ContainsSubstring;

// thuum docs/verbs/time.md. The clock's arithmetic and the 60 s resync are
// Rust's and tested there (cargo test -p wire-rules); these check the core's
// half: when the clock is sent, and what Papyrus reads from it.

namespace {
size_t CountGameTimeMessages(PartOne& partOne)
{
  size_t n = 0;
  for (auto& m : partOne.Messages()) {
    if (dynamic_cast<SetGameTimeMessage*>(m.message.get())) {
      ++n;
    }
  }
  return n;
}
}

TEST_CASE("A player hears the game clock at login, ahead of their own "
          "CreateActor",
          "[GameTime]")
{
  PartOne partOne;
  partOne.SetGameTimeSettings("{}");
  partOne.CreateActor(0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
  DoConnect(partOne, 0);
  partOne.SetUserActor(0, 0xff000ABC);

  REQUIRE(partOne.Messages().size() == 2);
  auto clock =
    dynamic_cast<SetGameTimeMessage*>(partOne.Messages().at(0).message.get());
  REQUIRE(clock);
  REQUIRE(partOne.Messages().at(0).reliable);
  REQUIRE(partOne.Messages().at(0).userId == 0);
  REQUIRE(partOne.Messages().at(0).j["t"] == 34);
  auto createActor =
    dynamic_cast<CreateActorMessage*>(partOne.Messages().at(1).message.get());
  REQUIRE(createActor);
  REQUIRE(createActor->isMe);

  auto now = partOne.GetGameTime();
  REQUIRE(clock->timeScale == 20.f);
  REQUIRE(clock->month <= 11);
  REQUIRE(clock->day >= 1);
  REQUIRE(clock->hour >= 0.f);
  REQUIRE(clock->hour < 24.f);
  REQUIRE(clock->daysPassed <= now.daysPassed);
  REQUIRE(now.daysPassed - clock->daysPassed < 0.01f);

  // the next one is a minute away
  partOne.Messages().clear();
  partOne.Tick();
  REQUIRE(CountGameTimeMessages(partOne) == 0);
}

TEST_CASE("Without its settings the core sends no clock", "[GameTime]")
{
  PartOne partOne;
  partOne.CreateActor(0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
  DoConnect(partOne, 0);
  partOne.SetUserActor(0, 0xff000ABC);
  REQUIRE(CountGameTimeMessages(partOne) == 0);
  partOne.Tick();
  REQUIRE(CountGameTimeMessages(partOne) == 0);
}

TEST_CASE("Bad time settings stop the server, named", "[GameTime]")
{
  PartOne partOne;
  REQUIRE_THROWS_WITH(partOne.SetGameTimeSettings(R"({"timeScale": 0})"),
                      ContainsSubstring("timeScale"));
  REQUIRE_THROWS_WITH(partOne.SetGameTimeSettings(R"({"timescale": 20})"),
                      ContainsSubstring("unknown field"));
}

TEST_CASE("Utility's time natives follow the game clock",
          "[GameTime][Papyrus][Utility]")
{
  WorldState wst;
  wst.gameTime = [] {
    GameTimeNow t;
    t.daysPassed = 42.5f;
    t.timeScale = 3600.f;
    return t;
  };
  PapyrusUtility utility;
  utility.compatibilityPolicy.reset(new HeuristicPolicy(&wst));

  REQUIRE(static_cast<double>(utility.GetCurrentGameTime(
            VarValue::None(), {})) == Catch::Approx(42.5));

  // at 3600 game seconds per real second a game hour is one real second,
  // so 0.03 game hours is 30 ms (the old 60 s per hour would be 1.8 s)
  bool waitFinished = false;
  utility.WaitGameTime(VarValue::None(), { VarValue(0.03f) })
    .Then([&](VarValue) { waitFinished = true; });
  wst.Tick();
  REQUIRE(!waitFinished);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  wst.Tick();
  REQUIRE(waitFinished);
}
