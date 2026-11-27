#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "SetWeatherMessage.h"

using Catch::Matchers::ContainsSubstring;

// thuum docs/verbs/weather.md (M4). The pick, the slots and the no-repeat
// rule are Rust's and tested there (cargo test -p wire-rules weather); these
// check the core's half: when a word is sent, to whom, what it says, and
// which settings the server refuses, named.

namespace {
std::vector<const SetWeatherMessage*> WeatherWords(PartOne& partOne,
                                                   Networking::UserId userId)
{
  std::vector<const SetWeatherMessage*> out;
  for (auto& m : partOne.Messages()) {
    if (m.userId != userId) {
      continue;
    }
    if (auto word = dynamic_cast<SetWeatherMessage*>(m.message.get())) {
      REQUIRE(m.j["t"] == 66);
      REQUIRE(m.reliable);
      out.push_back(word);
    }
  }
  return out;
}

// one weather over Tamriel (SkyrimCloudy, the climate's own), a slot of a
// game day: no change can fall inside a test
constexpr auto kOneCloudy =
  R"({"changeGameMinutes": 1440, "seed": 1, "pools": {"0x3c": [{"weather": "0x12f89", "chance": 100}]}})";
}

TEST_CASE("A player hears its worldspace's weather at login, once, and "
          "again after a relaunch",
          "[Weather]")
{
  PartOne partOne;
  partOne.SetWeatherSettings(kOneCloudy);
  partOne.SetWeatherEnabled(true);
  partOne.CreateActor(0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
  DoConnect(partOne, 0);
  partOne.SetUserActor(0, 0xff000ABC);

  auto words = WeatherWords(partOne, 0);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0]->place == 0x3c);
  REQUIRE(words[0]->weather == 0x12f89);
  REQUIRE_FALSE(words[0]->transition);

  // heard: the tick says nothing more
  partOne.Tick();
  REQUIRE(WeatherWords(partOne, 0).size() == 1);

  // a relaunch is a new ear
  DoDisconnect(partOne, 0);
  partOne.Messages().clear();
  DoConnect(partOne, 1);
  partOne.SetUserActor(1, 0xff000ABC);
  auto again = WeatherWords(partOne, 1);
  REQUIRE(again.size() == 1);
  REQUIRE(again[0]->weather == 0x12f89);
  REQUIRE_FALSE(again[0]->transition);
}

TEST_CASE("A place without a pool (an interior, a skyless worldspace) hears "
          "a release",
          "[Weather]")
{
  // a core without the game files resolves no game-file cell but Tamriel
  // (FormDesc::FromFormId's one workaround), so the place is a server-made
  // cell id: no pool, hence a release, as an interior gets
  constexpr uint32_t kInterior = 0xff000001;
  PartOne partOne;
  partOne.SetWeatherSettings(kOneCloudy);
  partOne.SetWeatherEnabled(true);
  partOne.CreateActor(0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, kInterior);
  DoConnect(partOne, 0);
  partOne.SetUserActor(0, 0xff000ABC);

  auto words = WeatherWords(partOne, 0);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0]->place == kInterior);
  REQUIRE(words[0]->weather == 0);
  REQUIRE_FALSE(words[0]->transition);
  partOne.Tick();
  REQUIRE(WeatherWords(partOne, 0).size() == 1);
}

TEST_CASE("Without the switch no weather is sent", "[Weather]")
{
  PartOne partOne;
  partOne.SetWeatherSettings(kOneCloudy);
  partOne.CreateActor(0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
  DoConnect(partOne, 0);
  partOne.SetUserActor(0, 0xff000ABC);
  partOne.Tick();
  REQUIRE(WeatherWords(partOne, 0).empty());
}

TEST_CASE("Bad weather settings stop the server, named", "[Weather]")
{
  PartOne partOne;
  REQUIRE_THROWS_WITH(
    partOne.SetWeatherSettings(R"({"changeGameMinutes": 0})"),
    ContainsSubstring("changeGameMinutes"));
  REQUIRE_THROWS_WITH(
    partOne.SetWeatherSettings(R"({"changeGameMinutes": 1441})"),
    ContainsSubstring("changeGameMinutes"));
  REQUIRE_THROWS_WITH(partOne.SetWeatherSettings(R"({"slots": 3})"),
                      ContainsSubstring("slots"));
  REQUIRE_THROWS_WITH(partOne.SetWeatherSettings(R"({"pools": {"0x3c": []}})"),
                      ContainsSubstring("0x3c"));
  REQUIRE_THROWS_WITH(
    partOne.SetWeatherSettings(
      R"({"pools": {"0x3c": [{"weather": "0x81a", "chance": 0}]}})"),
    ContainsSubstring("chance"));
  REQUIRE_THROWS_WITH(
    partOne.SetWeatherSettings(
      R"({"pools": {"0x3c": [{"weather": "0x81a", "chance": 5, "x": 1}]}})"),
    ContainsSubstring("'x'"));
  REQUIRE_NOTHROW(partOne.SetWeatherSettings("{}"));
}

// The engine holds a weather for its climate's own time before it draws the
// next: (255 - TNAM's volatility) * 22 / 255 + 1 game hours (TESClimate.h:60
// timing from TNAM, :43 volatility; the due check of the weather update,
// id 26231), 18.686 game hours for SkyrimClimate 0x812,
// whose volatility is 50. Without a slot named in the settings, a
// worldspace's slot is that time, a game minute inside the engine's own
// release of a forced weather: 1120 game minutes for Tamriel 0x3c.
extern espm::Loader& GetEspmLoader();

namespace {
// the Tamriel weather a fresh server's login word names, its clock held at
// `gameMinutes` of the game files' clock, under `weatherSettings`. The clock
// counts from its epoch at the time scale and reads its start before the
// epoch (wire-rules clock.rs), so an epoch after now holds it at the start,
// and a start hour of 0 adds nothing to daysPassed
uint32_t TamrielWeatherAt(double gameMinutes, const char* weatherSettings)
{
  static std::vector<std::shared_ptr<PartOne>> keep;
  auto partOne = std::make_shared<PartOne>();
  keep.push_back(partOne);
  const nlohmann::json timeSettings{
    { "epoch", "2999-01-01T00:00:00Z" },
    { "start", { { "hour", 0.0 }, { "daysPassed", gameMinutes / 1440.0 } } }
  };
  partOne->SetGameTimeSettings(timeSettings.dump());
  partOne->SetWeatherSettings(weatherSettings);
  partOne->SetWeatherEnabled(true);
  partOne->AttachEspm(&GetEspmLoader());
  partOne->CreateActor(0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
  DoConnect(*partOne, 0);
  partOne->SetUserActor(0, 0xff000ABC);
  const auto words = WeatherWords(*partOne, 0);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0]->place == 0x3c);
  REQUIRE(words[0]->weather != 0);
  return words[0]->weather;
}
}

TEST_CASE("Without a slot in the settings, Tamriel holds one weather for its "
          "climate's own time, 1120 game minutes, then draws another",
          "[Weather][espm]")
{
  // 33610 and 33730 game minutes: one 1120-minute slot (33600 to 34720), two
  // of the old default's 120-minute slots in a row, and 34730 the next slot
  const uint32_t first = TamrielWeatherAt(33610.0, R"({"seed": 1})");
  REQUIRE(TamrielWeatherAt(33730.0, R"({"seed": 1})") == first);
  REQUIRE(TamrielWeatherAt(34730.0, R"({"seed": 1})") != first);
  // a slot the settings name holds every worldspace
  REQUIRE(
    TamrielWeatherAt(33610.0, R"({"changeGameMinutes": 120, "seed": 1})") !=
    TamrielWeatherAt(33730.0, R"({"changeGameMinutes": 120, "seed": 1})"));
}
