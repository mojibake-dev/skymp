#include "ActionListener.h"
#include "GetBaseActorValues.h"
#include "RestIntentMessage.h"
#include "TestUtils.hpp"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>

PartOne& GetPartOne();

// thuum docs/verbs/rest.md: a player waits or sleeps for themselves; the
// server checks the rest (R1) and gives the player each attribute's
// regeneration over the rested hours (R0), the hours in game seconds
// (HYPOTHESIS until the verb's Dynamic plan measures the engine's base).

namespace {
constexpr uint32_t kActor = 0xff000abc;

MpActor& HalfPlayer(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kActor);
  auto& ac = p.worldState.GetFormAt<MpActor>(kActor);
  ac.SetPercentages({ 0.5f, 0.5f, 0.5f });
  p.Messages().clear();
  return ac;
}

void Rest(PartOne& p, float hours, bool sleep = false)
{
  RawMessageData rawMsgData;
  rawMsgData.userId = 0;
  RestIntentMessage msg;
  msg.hours = hours;
  msg.sleep = sleep;
  p.GetActionListener().OnRestIntent(rawMsgData, msg);
}

void Leave(PartOne& p)
{
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}

float Expected(float rate, float rateMult, float hours)
{
  return std::min(1.f, 0.5f + rate * rateMult / 10000.f * 3600.f * hours);
}
}

TEST_CASE("A rest gives the player its regeneration over the rested hours",
          "[Rest]")
{
  PartOne& p = GetPartOne();
  auto& ac = HalfPlayer(p);
  auto appearance = ac.GetAppearance();
  const BaseActorValues base = GetBaseActorValues(
    &p.worldState, ac.GetBaseId(), appearance ? appearance->raceId : 0, {});

  Rest(p, 1.f);

  const auto& values = ac.GetChangeForm().actorValues;
  REQUIRE_THAT(values.healthPercentage,
               Catch::Matchers::WithinAbs(
                 Expected(base.healRate, base.healRateMult, 1.f), 1e-5));
  REQUIRE_THAT(values.magickaPercentage,
               Catch::Matchers::WithinAbs(
                 Expected(base.magickaRate, base.magickaRateMult, 1.f), 1e-5));
  REQUIRE_THAT(values.staminaPercentage,
               Catch::Matchers::WithinAbs(
                 Expected(base.staminaRate, base.staminaRateMult, 1.f), 1e-5));
  // at the race's own rates an hour restores health fully (UESP,
  // Skyrim:Health: 142.86 s of regeneration to full)
  REQUIRE(values.healthPercentage == 1.f);

  auto& messages = p.Messages();
  REQUIRE(std::any_of(messages.begin(), messages.end(), [](auto& m) {
    return m.userId == 0 && m.j["t"] == MsgType::ChangeValues;
  }));
  Leave(p);
}

TEST_CASE("A rest is refused outside the menu's hours, while dead, or right "
          "after a hit",
          "[Rest]")
{
  PartOne& p = GetPartOne();
  auto& ac = HalfPlayer(p);
  const auto health = [&] {
    return ac.GetChangeForm().actorValues.healthPercentage;
  };

  Rest(p, 0.5f);
  Rest(p, 25.f);
  REQUIRE(health() == 0.5f);

  // a hit it took 3 s ago is a fight; 11 s ago is over
  using namespace std::chrono_literals;
  ac.SetLastHitTakenTime(std::chrono::steady_clock::now() - 3s);
  Rest(p, 8.f);
  REQUIRE(health() == 0.5f);
  ac.SetLastHitTakenTime(std::chrono::steady_clock::now() - 11s);
  Rest(p, 8.f);
  REQUIRE(health() == 1.f);
  Leave(p);

  auto& dead = HalfPlayer(p);
  dead.Kill();
  Rest(p, 8.f);
  REQUIRE(dead.IsDead());
  REQUIRE(dead.GetChangeForm().actorValues.healthPercentage < 1.f);
  Leave(p);
}

TEST_CASE("A server's rest switches turn waiting or sleeping off", "[Rest]")
{
  PartOne& p = GetPartOne();
  // the shared PartOne gets its defaults back however this case ends
  struct Restore
  {
    PartOne& p;
    ~Restore() { p.SetRestSettings("{}"); }
  } restore{ p };
  p.SetRestSettings(R"({"allowWait": false})");
  auto& ac = HalfPlayer(p);
  const auto health = [&] {
    return ac.GetChangeForm().actorValues.healthPercentage;
  };

  Rest(p, 8.f);
  REQUIRE(health() == 0.5f);
  Rest(p, 8.f, true);
  REQUIRE(health() == 1.f);
  Leave(p);

  REQUIRE_THROWS(p.SetRestSettings(R"({"allowWildernessRest": true})"));
}
