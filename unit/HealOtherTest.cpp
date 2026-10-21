#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "HitMessage.h"
#include "MpActor.h"
#include "MsgType.h"
#include "wire_bridge_cxx/rules.h"
#include <chrono>
#include <thread>

PartOne& GetPartOne();
using namespace std::chrono_literals;

// thuum docs/verbs/magic-effects.md (train-31; playtest twenty-eight, Eli:
// "i should be able to cast healing hands on them"): Healing Hands
// (Skyrim.esm 0x4D3F2, RestoreHealthConcActor 0x1CEA5 at 10 a second on
// another actor, its conditions HasKeyword and IsUndead) heals the player it
// is cast on through the server, as a hostile effect's damage is dealt, and
// begins no fight between the two: ADR-023's hostility comes from a hostile
// or detrimental effect only. Firebolt (0x12FD0) still begins one. The
// spell's Respite slot (0x4E93E, HasPerk) is refused as ADR-039 refuses a
// perk, with nothing lost: the record gives a player without the perk
// nothing either.
namespace {
constexpr uint32_t kHealingHands = 0x0004d3f2;
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;

// a cast through the packet path, as a client's arrives, the caster naming
// itself 0x14 (unit/SpellCastTest.cpp's shape)
void Cast(PartOne& p, uint32_t spell)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", kTarget },
                        { "spell", spell },
                        { "isDualCasting", false },
                        { "interruptCast", false },
                        { "castingSource", 1 },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "hasLaunch", false },
                        { "originX", 0.f },
                        { "originY", 0.f },
                        { "originZ", 0.f },
                        { "launchAngleX", 0.f },
                        { "launchAngleZ", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
}
}

TEST_CASE("Healing Hands on another player heals it on the server and begins "
          "no fight; Firebolt begins one",
          "[HealOther]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kCaster, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kCaster);
  p.CreateActor(kTarget, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(target.SetActorValueRecord(record));
  target.SetPercentages({ 0.5f, 1.f, 1.f });

  // the StartCombat snippets the users received since the last clear
  const auto startCombats = [&] {
    p.Tick(); // snippets are deferred
    size_t count = 0;
    for (auto& m : p.Messages()) {
      if (m.j["t"] == MsgType::SpSnippet && m.j["function"] == "StartCombat") {
        ++count;
      }
    }
    return count;
  };
  // the spell's hit on the target, after its cast (the caller equips, casts
  // and clears the messages: a stream's hit counts the time since the cast)
  const auto hit = [&](uint32_t spell) {
    RawMessageData raw;
    raw.userId = 0;
    HitMessage msg;
    msg.data.aggressor = 0x14;
    msg.data.target = kTarget;
    msg.data.source = spell;
    p.GetActionListener().OnHit(raw, msg);
  };
  const auto health = [&] {
    return target.GetChangeForm().actorValues.healthPercentage;
  };

  // the stream's hit counts the time it held its target since the cast
  // (wire-rules casts MAX_TICK_MS 250; unit/SpellCastTest.cpp's Flames):
  // 10 a second for 0.12 s to 0.25 s, 1.2 to 2.5 of 250; and no fight
  Equipment eq;
  eq.rightSpell = kHealingHands;
  caster.SetEquipment(eq);
  Cast(p, kHealingHands);
  std::this_thread::sleep_for(120ms);
  p.Messages().clear();
  hit(kHealingHands);
  const float healed = health();
  REQUIRE(healed >= 0.5f + 1.1f / 250.f);
  REQUIRE(healed <= 0.5f + 2.6f / 250.f);
  REQUIRE(startCombats() == 0);
  REQUIRE(!p.GetFights().in_fight(kTarget));

  // a hostile hit: the fire damage (25 of 250) lands and the fight begins
  eq.rightSpell = kFirebolt;
  caster.SetEquipment(eq);
  Cast(p, kFirebolt);
  p.Messages().clear();
  hit(kFirebolt);
  REQUIRE(health() < healed - 20.f / 250.f);
  REQUIRE(startCombats() == 1);
  REQUIRE(p.GetFights().in_fight(kTarget));

  p.DestroyActor(kCaster);
  p.DestroyActor(kTarget);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}
