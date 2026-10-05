#include "ActionListener.h"
#include "ActivateMessage.h"
#include "GetBaseActorValues.h"
#include "RestIntentMessage.h"
#include "TestUtils.hpp"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>

PartOne& GetPartOne();

// thuum docs/verbs/rest.md: a player waits or sleeps for themselves; the
// server checks the rest (R1) and gives the player each attribute's
// regeneration over the rested hours (R0): 360 seconds of it a game hour, as
// the engine gives at time scale 20 (measured, runs 20261004-095556 and
// 20261004-100002).

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

// thuum docs/verbs/sleep.md: the hunters' camp bedroll southwest of the
// lab spawn, Skyrim.esm REFR 0x000B3184 at (117786, -81333, 10997) in
// Tamriel, unowned and touched by no DLC master (lab/esm.py, 2026-10-04)
constexpr uint32_t kBed = 0x000B3184;
const NiPoint3 kAtBed{ 117786, -81400, 10997 };

// Whether the user's activation of the bed was let through to its game
bool ActivateBed(PartOne& p, Networking::UserId user)
{
  p.Messages().clear();
  RawMessageData raw;
  raw.userId = user;
  ActivateMessage msg;
  msg.data.caster = 0x14;
  msg.data.target = kBed;
  msg.data.isSecondActivation = false;
  p.GetActionListener().OnActivate(raw, msg);
  auto& m = p.Messages();
  return std::any_of(m.begin(), m.end(), [&](auto& x) {
    return x.userId == user && x.j["t"] == MsgType::OpenContainer;
  });
}

void Leave(PartOne& p)
{
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}

float Expected(float rate, float rateMult, float hours)
{
  return std::min(1.f, 0.5f + rate * rateMult / 10000.f * 360.f * hours);
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
  // thuum docs/verbs/sleep.md: the client's flag does not make a sleep
  Rest(p, 8.f, true);
  REQUIRE(health() == 0.5f);
  // a rest at a bed the player just activated does, and sleeping stays on
  ac.SetPos(kAtBed);
  REQUIRE(ActivateBed(p, 0));
  Rest(p, 8.f);
  REQUIRE(health() == 1.f);
  Leave(p);

  REQUIRE_THROWS(p.SetRestSettings(R"({"allowWildernessRest": true})"));
}

TEST_CASE("A rest at a bed just activated is a sleep: the bed works again "
          "and Rested is granted; a wait away from it grants nothing",
          "[Rest]")
{
  // thuum docs/verbs/sleep.md, at the hunters' camp bedroll; Rested is
  // Skyrim.esm SPEL 0x000FB981
  constexpr uint32_t kOther = 0xff000abd;
  PartOne& p = GetPartOne();
  auto& ac = HalfPlayer(p);
  ac.SetPos(kAtBed);
  DoConnect(p, 1);
  p.CreateActor(kOther, { 117820, -81400, 10997 }, 0, 0x3c);
  p.SetUserActor(1, kOther);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);

  const auto activate = [&](Networking::UserId user) {
    return ActivateBed(p, user);
  };
  // the Rested grants user 0 received
  const auto restedGrants = [&] {
    p.Tick(); // snippets are deferred
    auto& m = p.Messages();
    return std::count_if(m.begin(), m.end(), [](auto& x) {
      return x.userId == 0 && x.j["t"] == MsgType::SpSnippet &&
        x.j["function"] == "AddSpell" &&
        x.j["arguments"][0]["formId"] == 0x000FB981;
    });
  };

  REQUIRE(activate(0));
  REQUIRE(activate(0));  // its occupant may use it again
  REQUIRE(!activate(1)); // another player may not while it is held

  p.Messages().clear();
  Rest(p, 1.f, false); // the client's flag says a wait; the server knows
  REQUIRE(ac.GetChangeForm().actorValues.healthPercentage == 1.f);
  REQUIRE(restedGrants() == 1);
  REQUIRE(ac.GetLastBed().first == 0);
  REQUIRE(activate(1)); // the rest released the bed

  // the other player walks off; a rest 1400 units from the bed is a wait
  other.SetPos({ 117820, -79000, 10997 });
  REQUIRE(activate(0));
  ac.SetPercentages({ 0.5f, 0.5f, 0.5f });
  ac.SetPos({ 117786, -80000, 10997 });
  p.Messages().clear();
  Rest(p, 1.f, true); // nor does the client's flag make it a sleep
  REQUIRE(restedGrants() == 0);

  p.DestroyActor(kOther);
  DoDisconnect(p, 1);
  Leave(p);
}

TEST_CASE("A player in a fight with another player cannot rest until it "
          "ends",
          "[Rest]")
{
  // thuum ADR-023 and its amendment: a fight ends a minute without a hit or
  // with the players apart; until then the server refuses a rest, however
  // long ago its last hit
  constexpr uint32_t kOther = 0xff000abe;
  PartOne& p = GetPartOne();
  auto& ac = HalfPlayer(p);
  DoConnect(p, 1);
  p.CreateActor(kOther, { 50, 0, 0 }, 0, 0x3c);
  p.SetUserActor(1, kOther);
  const auto nowMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());

  REQUIRE(p.GetFights().hit(kOther, kActor, nowMs - 45'000));
  Rest(p, 1.f);
  REQUIRE(ac.GetChangeForm().actorValues.healthPercentage == 0.5f);

  p.TickFights(nowMs + 16'000); // a minute after its last hit
  REQUIRE(!p.GetFights().in_fight(kActor));
  Rest(p, 1.f);
  REQUIRE(ac.GetChangeForm().actorValues.healthPercentage == 1.f);

  p.DestroyActor(kOther);
  DoDisconnect(p, 1);
  Leave(p);
}
