#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "HitMessage.h"
#include "MsgType.h"
#include "UpdateMovementMessage.h"

PartOne& GetPartOne();

// thuum docs/verbs/shadowbond.md: a spell the server names as a bond
// (server-settings.json bonds) bonds its caster and the actor its claimed
// hit lands on; the first act that ends either's concealment breaks the
// bond and permits each one jump to where the other stands, within the
// entry's slack, for its window. Skyrim.esm's dunHonningbrewSkeeverPoison
// (0x10FC22, delivered by touch; thuum lab/esm.py 2026-10-18) stands in
// for Apocalypse's Shadowbond, which the test's load order lacks.
namespace {
constexpr uint32_t kTouch = 0x0010fc22;
// Skyrim.esm Flames, the act that breaks the bond in x-shadowbond
constexpr uint32_t kFlames = 0x00012fcd;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;
constexpr auto kBonds = R"([{"spell": "10fc22:Skyrim.esm"}])";

// The caster (user 0) at the origin with the spell in its left hand, and
// the target (user 1) 300 units north, both in Tamriel
std::pair<MpActor*, MpActor*> Pair(PartOne& p)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kCaster, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kCaster);
  p.CreateActor(kTarget, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  Equipment eq;
  eq.leftSpell = kTouch;
  caster.SetEquipment(eq);
  return { &caster, &p.worldState.GetFormAt<MpActor>(kTarget) };
}

void Cast(PartOne& p, uint32_t spell = kTouch, int castingSource = 0)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", 0 },
                        { "spell", spell },
                        { "isDualCasting", false },
                        { "interruptCast", false },
                        { "castingSource", castingSource },
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

void Hit(PartOne& p)
{
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kTarget;
  hit.data.source = kTouch;
  p.GetActionListener().OnHit(raw, hit);
}

// a player's own movement report, from its user
void Report(PartOne& p, Networking::UserId user, MpActor& me,
            const NiPoint3& pos)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = me.GetIdx();
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = 0x3c;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

int Sent(PartOne& p, Networking::UserId user, MsgType type)
{
  int n = 0;
  for (auto& m : p.Messages()) {
    n += m.userId == user && m.j.contains("t") &&
      m.j["t"] == static_cast<int>(type);
  }
  return n;
}

void Leave(PartOne& p)
{
  p.SetBondSettings("[]");
  p.DestroyActor(kTarget);
  p.DestroyActor(kCaster);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A bond spell's claimed hit bonds the two, and the break permits "
          "each a jump to where the other stands",
          "[Bond][espm]")
{
  PartOne& p = GetPartOne();
  p.SetBondSettings(kBonds);
  auto [caster, target] = Pair(p);
  REQUIRE(p.IsBondSpell(kTouch));

  // no bond yet: an act breaks nothing
  REQUIRE_FALSE(p.BreakBondsOf(kCaster));

  Cast(p);
  Hit(p);
  p.Messages().clear();
  REQUIRE(p.BreakBondsOf(kTarget));
  // the bond is spent
  REQUIRE_FALSE(p.BreakBondsOf(kCaster));

  // the server swaps their places itself: each game is told its move (the
  // caster's own script lands on the same spot; the target's game runs no
  // script at all, x-shadowbond 20261019-073758)
  REQUIRE(Sent(p, 0, MsgType::Teleport) == 1);
  REQUIRE(Sent(p, 1, MsgType::Teleport) == 1);
  REQUIRE(caster->GetPos() == NiPoint3{ 0, 300, 0 });
  REQUIRE(target->GetPos() == NiPoint3{ 0, 0, 0 });

  // each game's report from where it was put is the arrival: taken, and
  // placed for the other game
  p.Messages().clear();
  Report(p, 0, *caster, { 0, 300, 0 });
  REQUIRE(Sent(p, 0, MsgType::Teleport2) == 0);
  REQUIRE(Sent(p, 1, MsgType::ActorJump) == 1);
  REQUIRE(caster->GetPos() == NiPoint3{ 0, 300, 0 });
  p.Messages().clear();
  Report(p, 1, *target, { 0, 0, 0 });
  REQUIRE(Sent(p, 1, MsgType::Teleport2) == 0);
  REQUIRE(Sent(p, 0, MsgType::ActorJump) == 1);
  REQUIRE(target->GetPos() == NiPoint3{ 0, 0, 0 });

  // the mod's script in the caster's game moves the caster once more, to
  // where the target's figure now stands, its own old spot (x-shadowbond
  // 20261019-083537): within the hold that report is sent back to the
  // swap's place, and the server's position stands
  p.Messages().clear();
  Report(p, 0, *caster, { 0, 0, 0 });
  REQUIRE(Sent(p, 0, MsgType::Teleport2) == 1);
  REQUIRE(caster->GetPos() == NiPoint3{ 0, 300, 0 });
  Leave(p);
}

TEST_CASE("A bonded actor's report from elsewhere after the swap is dropped, "
          "the server's move standing",
          "[Bond][espm]")
{
  PartOne& p = GetPartOne();
  p.SetBondSettings(kBonds);
  auto [caster, target] = Pair(p);
  Cast(p);
  Hit(p);
  REQUIRE(p.BreakBondsOf(kCaster));
  p.Messages().clear();
  // 3000 units east: not where the server put it; sent before the move as
  // far as the arrival rule knows, so dropped, not sent back (the reliable
  // teleport is on its way)
  Report(p, 0, *caster, { 3000, 0, 0 });
  REQUIRE(Sent(p, 0, MsgType::Teleport2) == 0);
  REQUIRE(Sent(p, 1, MsgType::ActorJump) == 0);
  REQUIRE(caster->GetPos() == NiPoint3{ 0, 300, 0 });
  (void)target;
  Leave(p);
}

TEST_CASE("A bonded actor's own cast breaks the bond, its concealment "
          "recorded or not",
          "[Bond][espm]")
{
  PartOne& p = GetPartOne();
  p.SetBondSettings(kBonds);
  auto [caster, target] = Pair(p);
  Cast(p);
  Hit(p);
  // the caster's invisibility is the mod's script's, in its game alone
  // (ADR-029), so the server records none for it; its next cast is the act
  // (x-shadowbond 20261019-081315: bond recorded, Flames cast, no break)
  Equipment eq;
  eq.leftSpell = kTouch;
  eq.rightSpell = kFlames;
  caster->SetEquipment(eq);
  p.Messages().clear();
  Cast(p, kFlames, 1);
  REQUIRE(Sent(p, 0, MsgType::Teleport) == 1);
  REQUIRE(Sent(p, 1, MsgType::Teleport) == 1);
  REQUIRE(caster->GetPos() == NiPoint3{ 0, 300, 0 });
  REQUIRE(target->GetPos() == NiPoint3{ 0, 0, 0 });
  REQUIRE_FALSE(p.BreakBondsOf(kCaster));
  Leave(p);
}

TEST_CASE("A spell the server names no bond bonds nobody", "[Bond][espm]")
{
  PartOne& p = GetPartOne();
  p.SetBondSettings("[]");
  auto [caster, target] = Pair(p);
  REQUIRE_FALSE(p.IsBondSpell(kTouch));
  Cast(p);
  Hit(p);
  REQUIRE_FALSE(p.BreakBondsOf(kCaster));
  REQUIRE_FALSE(p.BreakBondsOf(kTarget));
  (void)caster;
  (void)target;
  Leave(p);
}
