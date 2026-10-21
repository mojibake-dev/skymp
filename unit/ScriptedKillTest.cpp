#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "HitMessage.h"
#include "MpActor.h"
#include "MsgType.h"

PartOne& GetPartOne();

// thuum docs/verbs/scripted-effects.md (train-31, ADR-029's first allowlist
// entry; playtest twenty-eight, Eli: "if it kills them by effect of the
// spell, yes kill them"): a spell server-settings.json's `scriptedEffects`
// names as Finger of Death's kill has its script's arithmetic run by the
// server from the claimed hit (docs/research/train-31-reads.md section 5):
// the caster takes 125 percent of the target's Health; when that leaves the
// caster alive the target loses its whole Health and dies by the caster's
// hand, else the caster drops to one point and the target loses that much
// scaled back by the percentage, and nobody dies. Firebolt stands in for
// the mod's spell here (Skyrim.esm; the kill is the table's, the Script
// archetype Apocalypse's), its own 25 fire damage landing after the blast.
namespace {
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;

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

TEST_CASE("A scripted kill: the server runs Finger of Death's arithmetic from "
          "the claimed hit, in both branches",
          "[ScriptedKill]")
{
  PartOne& p = GetPartOne();
  p.SetScriptedEffectSettings(
    R"([{"spell": "12fd0:Skyrim.esm", "kill": "fingerOfDeath"}])");
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
  REQUIRE(caster.SetActorValueRecord(record));
  REQUIRE(target.SetActorValueRecord(record));
  Equipment eq;
  eq.rightSpell = kFirebolt;
  caster.SetEquipment(eq);

  const auto hit = [&] {
    Cast(p, kFirebolt);
    RawMessageData raw;
    raw.userId = 0;
    HitMessage msg;
    msg.data.aggressor = 0x14;
    msg.data.target = kTarget;
    msg.data.source = kFirebolt;
    p.GetActionListener().OnHit(raw, msg);
  };
  const auto health = [](MpActor& actor) {
    return actor.GetChangeForm().actorValues.healthPercentage;
  };

  // the fail branch: a target at 250 would cost the caster 312.5, over its
  // 249: the caster drops to one point, the target takes 249 * 0.8 = 199.2
  // and then Firebolt's 25; nobody dies
  caster.SetPercentages({ 1.f, 1.f, 1.f });
  target.SetPercentages({ 1.f, 1.f, 1.f });
  hit();
  REQUIRE(!target.GetChangeForm().isDead);
  REQUIRE(health(target) ==
          Catch::Approx((250.f - 199.2f - 25.f) / 250.f).margin(0.01f));
  REQUIRE(!caster.GetChangeForm().isDead);
  // the caster's share is its own game's (the mod's script runs there and
  // reports the fall; b-fod-kill 20261021-042635 saw the server's share and
  // the script's stack to a death), so a player caster stays whole here
  REQUIRE(health(caster) == Catch::Approx(1.f));

  // the kill branch: a target at 125 costs the caster 156.25, under its
  // 249: the target dies by the caster's hand and the caster keeps 93.75
  caster.SetPercentages({ 1.f, 1.f, 1.f });
  target.SetPercentages({ 0.5f, 1.f, 1.f });
  hit();
  REQUIRE(target.GetChangeForm().isDead);
  REQUIRE(!caster.GetChangeForm().isDead);
  REQUIRE(health(caster) == Catch::Approx(1.f));

  p.SetScriptedEffectSettings("[]");
  p.DestroyActor(kCaster);
  p.DestroyActor(kTarget);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}
