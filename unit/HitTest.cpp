#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>

#include "GetBaseActorValues.h"
#include "HitMessage.h"
#include "PacketParser.h"
#include "libespm/Loader.h"

PartOne& GetPartOne();
extern espm::Loader l;
using namespace std::chrono_literals;

namespace {
const auto kExtraWornTrue = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();
}

TEST_CASE("OnHit damages target actor based on damage formula", "[Hit]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);

  RawMessageData rawMsgData;
  rawMsgData.userId = 0;
  HitMessage hitMsg;
  hitMsg.data.target = 0x14;
  hitMsg.data.aggressor = 0x14;
  hitMsg.data.source = 0x0001397E; // iron dagger 4 damage, id = 80254
  ac.AddItem(hitMsg.data.source, 1);

  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(80254, 1, kExtraWornTrue));
  ac.SetEquipment(eq);

  auto past = std::chrono::steady_clock::now() - 10s;
  ac.SetLastHitTime(0xff000000, past);
  p.Messages().clear();
  p.GetActionListener().OnHit(rawMsgData, hitMsg);

  REQUIRE(p.Messages().size() == 1);
  auto changeForm = ac.GetChangeForm();
  REQUIRE(changeForm.actorValues.healthPercentage == 0.75f);
  REQUIRE(changeForm.actorValues.magickaPercentage == 1.f);
  REQUIRE(changeForm.actorValues.staminaPercentage == 1.f);

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("OnHit function sends ChangeValues message with coorect percentages",
          "[TES5DamageFormula]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  ac.SetEquipment(Equipment());

  RawMessageData rawMsgData;
  rawMsgData.userId = 0;
  HitMessage hitMsg;
  hitMsg.data.target = 0x14;
  hitMsg.data.aggressor = 0x14;
  hitMsg.data.source = 0x0001397E; // iron dagger 4 damage
  ac.AddItem(hitMsg.data.source, 1);

  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(80254, 1, kExtraWornTrue));
  ac.SetEquipment(eq);

  p.Messages().clear();
  auto past = std::chrono::steady_clock::now() - 4s;
  ac.SetLastHitTime(0xff000000, past);
  p.GetActionListener().OnHit(rawMsgData, hitMsg);

  REQUIRE(p.Messages().size() == 1);
  nlohmann::json message = p.Messages()[0].j;

  REQUIRE(message["data"]["health"] == 0.75f);
  REQUIRE(message["data"]["magicka"] == nlohmann::json{});
  REQUIRE(message["data"]["stamina"] == nlohmann::json{});

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("OnHit doesn't damage character if it is out of range", "[Hit]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  RawMessageData rawMsgData;
  rawMsgData.userId = 0;

  const uint32_t aggressor = 0xff000000;
  const uint32_t target = 0xff000001;

  p.CreateActor(aggressor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, aggressor);
  auto& acAggressor = p.worldState.GetFormAt<MpActor>(aggressor);

  p.CreateActor(target, { 0, 0, 0 }, 0, 0x3c);
  auto& acTarget = p.worldState.GetFormAt<MpActor>(target);

  HitMessage hitMsg;
  hitMsg.data.target = target;
  hitMsg.data.aggressor = 0x14;
  hitMsg.data.source = 0x0001397E;

  int16_t face =
    espm::GetData<espm::NPC_>(acAggressor.GetBaseId(), &p.worldState)
      .objectBounds.pos2[1];
  int16_t targetSide =
    espm::GetData<espm::NPC_>(acTarget.GetBaseId(), &p.worldState)
      .objectBounds.pos2[1];

  // fCombatDistance global value * reach
  const float awaitedRange = 141.f * 0.7f + face + targetSide;
  acTarget.SetPos({ awaitedRange * 1.001f, 0, 0 });
  acTarget.SetAngle({ 0.f, 0.f, 180.f });
  ActorValues actorValues;
  actorValues.healthPercentage = 0.1f;
  actorValues.magickaPercentage = 1.f;
  actorValues.staminaPercentage = 1.f;
  acTarget.SetPercentages(actorValues);

  auto past = std::chrono::steady_clock::now() - 2s;
  acTarget.SetLastHitTime(target, past);
  acAggressor.SetLastHitTime(target, past);
  p.GetActionListener().OnHit(rawMsgData, hitMsg);

  auto changeForm = acTarget.GetChangeForm();
  REQUIRE(changeForm.actorValues.healthPercentage == 0.1f);

  p.DestroyActor(aggressor);
  p.DestroyActor(target);
  DoDisconnect(p, 0);
}

TEST_CASE("Dead actors can't attack", "[Hit]")
{
  PartOne& p = GetPartOne();
  RawMessageData rawMsgData;

  const uint32_t aggressor = 0xff000000;
  const uint32_t target = 0xff000001;

  p.CreateActor(aggressor, { 0, 0, 0 }, 0, 0x3c);
  p.CreateActor(target, { 0, 0, 0 }, 0, 0x3c);

  DoConnect(p, 0);
  p.SetUserActor(0, aggressor);
  rawMsgData.userId = 0;

  HitMessage hitMsg;
  hitMsg.data.target = target;
  hitMsg.data.aggressor = 0x14;
  hitMsg.data.source = 0x0001397E;

  auto& acTarget = p.worldState.GetFormAt<MpActor>(target);
  ActorValues actorValues;
  actorValues.healthPercentage = 0.2f;
  actorValues.magickaPercentage = 1.f;
  actorValues.staminaPercentage = 1.f;
  acTarget.SetPercentages(actorValues);

  auto& acAggressor = p.worldState.GetFormAt<MpActor>(aggressor);
  acAggressor.Kill();
  REQUIRE(acAggressor.IsDead() == true);

  p.GetActionListener().OnHit(rawMsgData, hitMsg);

  REQUIRE(acTarget.GetChangeForm().actorValues.healthPercentage == 0.2f);

  p.DestroyActor(aggressor);
  p.DestroyActor(target);
  DoDisconnect(p, 0);
}

TEST_CASE("checking weapon cooldown", "[Hit]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);

  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);

  ActorValues actorValues;
  actorValues.healthPercentage = 1.f;
  actorValues.magickaPercentage = 1.f;
  actorValues.staminaPercentage = 1.f;
  ac.SetPercentages(actorValues);

  RawMessageData msgData;
  msgData.userId = 0;
  HitMessage hitMsg;
  hitMsg.data.target = 0x14;
  hitMsg.data.aggressor = 0x14;
  hitMsg.data.source = 0x0001397E;
  ac.AddItem(hitMsg.data.source, 1);

  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(80254, 1, kExtraWornTrue));
  ac.SetEquipment(eq);

  auto past = std::chrono::steady_clock::now() - 300ms;

  ac.SetLastHitTime(0xff000000, past);
  p.Messages().clear();
  p.GetActionListener().OnHit(msgData, hitMsg);

  auto current = ac.GetLastHitTime(0xff000000);
  std::chrono::duration<float> duration = current - past;
  float passedTime = duration.count();
  float daggerSpeed = 1.3f;

  REQUIRE(passedTime <= 1.1 * (1 / daggerSpeed));
  REQUIRE(p.Messages().size() == 0);

  past = std::chrono::steady_clock::now() - 3s;
  ac.SetLastHitTime(0xff000000, past);
  p.Messages().clear();
  p.GetActionListener().OnHit(msgData, hitMsg);
  current = ac.GetLastHitTime(0xff000000);
  duration = current - past;
  passedTime = duration.count();

  REQUIRE(passedTime >= 1.1 * (1 / daggerSpeed));
  REQUIRE(p.Messages().size() == 1);
  nlohmann::json message = p.Messages()[0].j;
  uint64_t msgType = 16; // OnHit sends ChangeValues message type
  REQUIRE(message["t"] == msgType);
  REQUIRE(message["data"]["health"] == 0.75f);
  REQUIRE(message["data"]["magicka"] == nlohmann::json{});
  REQUIRE(message["data"]["stamina"] == nlohmann::json{});

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("A melee hit on a player from beyond reach does nothing", "[Hit]")
{
  // thuum docs/verbs/melee-reach.md: an iron sword (reach 1.0) on a Nord
  // (height 1.03) reaches max(141 * 1.03, 162) = 162, plus both forward
  // extents (14 * 1.03 each) and 256 for stale positions: about 447 units.
  // Player against player only: a target no user plays keeps the old bound.
  // The aggressor faces north (+y) and every target stands on its heading,
  // so the hit cone (thuum docs/verbs/hit-cone.md) never decides here; the
  // NPC behind it shows the cone is a player-on-player rule too.
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  DoConnect(p, 1);
  const uint32_t aggressor = 0xff000000;
  const uint32_t target = 0xff000001;
  const uint32_t npc = 0xff000002;
  p.CreateActor(aggressor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, aggressor);
  p.CreateActor(target, { 0, 400, 0 }, 0, 0x3c);
  p.SetUserActor(1, target);
  p.CreateActor(npc, { 0, -2000, 0 }, 0, 0x3c);
  auto& acAggressor = p.worldState.GetFormAt<MpActor>(aggressor);

  const uint32_t ironSword = 0x00012eb7;
  acAggressor.AddItem(ironSword, 1);
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(ironSword, 1, kExtraWornTrue));
  acAggressor.SetEquipment(eq);

  RawMessageData rawMsgData;
  rawMsgData.userId = 0;
  const auto hitFrom = [&](uint32_t victimId, NiPoint3 pos) {
    auto& victim = p.worldState.GetFormAt<MpActor>(victimId);
    victim.SetPos(pos);
    ActorValues full;
    full.healthPercentage = 1.f;
    full.magickaPercentage = 1.f;
    full.staminaPercentage = 1.f;
    victim.SetPercentages(full);
    // no weapon cooldown or splash window between the cases
    for (uint32_t id : { target, npc }) {
      acAggressor.SetLastHitTime(id, std::chrono::steady_clock::now() - 10s);
    }
    HitMessage hitMsg;
    hitMsg.data.aggressor = 0x14;
    hitMsg.data.target = victimId;
    hitMsg.data.source = ironSword;
    p.GetActionListener().OnHit(rawMsgData, hitMsg);
    return victim.GetChangeForm().actorValues.healthPercentage < 1.f;
  };

  REQUIRE(hitFrom(target, { 0, 400, 0 }));
  REQUIRE(!hitFrom(target, { 0, 500, 0 }));
  REQUIRE(!hitFrom(target, { 0, 2000, 0 }));
  REQUIRE(hitFrom(npc, { 0, -2000, 0 }));

  // thuum docs/verbs/hit-cone.md: within reach but behind the aggressor
  // (180 degrees off its heading, past any bound: 95 for a Nord, 80 for a
  // race without attack data, 145 for a Nord's sweep) the hit is refused;
  // 30 degrees off, where the lab's swings landed, it lands
  REQUIRE(!hitFrom(target, { 0, -100, 0 }));
  REQUIRE(hitFrom(target, { 50, 86.6f, 0 }));

  p.DestroyActor(aggressor);
  p.DestroyActor(target);
  p.DestroyActor(npc);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}

namespace {
// Damage stays fixed; what the formula was told is what the test reads.
class FlagRecordingFormula : public IDamageFormula
{
public:
  explicit FlagRecordingFormula(std::shared_ptr<HitData> seen_)
    : seen(std::move(seen_))
  {
  }

  [[nodiscard]] float CalculateDamage(const MpActor&, const MpActor&,
                                      const HitData& hitData) const override
  {
    *seen = hitData;
    return 1.f;
  }

  [[nodiscard]] float CalculateDamage(const MpActor&, const MpActor&,
                                      const SpellCastData&) const override
  {
    return 1.f;
  }

private:
  std::shared_ptr<HitData> seen;
};
}

TEST_CASE("A player's power and sneak flags count only when the server saw "
          "them",
          "[Hit]")
{
  // thuum docs/verbs/damage-flags.md: a power attack needs a power attack's
  // start among the attacker's animation events within three seconds, a
  // sneak attack the attacker's own sneaking state; otherwise the flag is
  // dropped and the hit lands as a plain one
  PartOne& p = GetPartOne();
  auto seen = std::make_shared<HitData>();
  p.SetDamageFormula(std::make_unique<FlagRecordingFormula>(seen));
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);

  RawMessageData rawMsgData;
  rawMsgData.userId = 0;
  const auto hit = [&](bool power, bool sneak) {
    ac.SetLastHitTime(0xff000000, std::chrono::steady_clock::now() - 10s);
    HitMessage hitMsg;
    hitMsg.data.aggressor = 0x14;
    hitMsg.data.target = 0x14;
    hitMsg.data.source = 0x1f4; // bare hands
    hitMsg.data.isPowerAttack = power;
    hitMsg.data.isSneakAttack = sneak;
    *seen = HitData();
    p.GetActionListener().OnHit(rawMsgData, hitMsg);
    return *seen;
  };

  REQUIRE(!hit(true, false).isPowerAttack);
  AnimationData powerAttack;
  powerAttack.animEventName = "attackPowerStartForward";
  p.animationSystem.Process(&ac, powerAttack);
  REQUIRE(hit(true, false).isPowerAttack);

  REQUIRE(!hit(false, true).isSneakAttack);
  ac.SetAnimationVariableBool(AnimationVariableBool::kVariable_IsSneaking,
                              true);
  REQUIRE(hit(false, true).isSneakAttack);

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}
