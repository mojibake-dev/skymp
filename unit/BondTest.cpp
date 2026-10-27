#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "HitMessage.h"
#include "MsgType.h"
#include "UpdateMovementMessage.h"

#include <chrono>
#include <thread>

PartOne& GetPartOne();

// thuum docs/verbs/shadowbond.md: a spell the server names as a bond
// (server-settings.json bonds) bonds its caster and the actor its claimed
// hit lands on; the first act that ends either's concealment breaks the
// bond and swaps the two. Skyrim.esm's dunHonningbrewSkeeverPoison
// (0x10FC22, delivered by touch; thuum lab/esm.py 2026-10-18) stands in
// for Apocalypse's Shadowbond, which the test's load order lacks, and
// Skyrim.esm's Invisibility (0x27EB6, InvisibillityFFSelf, archetype
// Invisibility; SelfCastTest) for the mod's proc, the concealment the
// server records on both and casts in every game but the caster's
// (train-28).
namespace {
constexpr uint32_t kTouch = 0x0010fc22;
constexpr uint32_t kProc = 0x00027eb6;
// Skyrim.esm Flames, the act that breaks the bond in x-shadowbond
constexpr uint32_t kFlames = 0x00012fcd;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kTarget = 0xff000001;
constexpr auto kBonds =
  R"([{"spell": "10fc22:Skyrim.esm", "proc": "27eb6:Skyrim.esm"}])";
constexpr auto kInvisibility = espm::MGEF::EffectType::Invisibility;

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

// the SpSnippets `cls`.`fn` sent to `user` (on `selfId` when not 0; a
// snippet on the user's own actor travels on 0x14, SpSnippet.cpp)
int Snippets(PartOne& p, Networking::UserId user, const char* cls,
             const char* fn, uint32_t selfId = 0)
{
  int n = 0;
  for (auto& m : p.Messages()) {
    n += m.userId == user && m.j.contains("t") &&
      m.j["t"] == static_cast<int>(MsgType::SpSnippet) &&
      m.j["class"] == cls && m.j["function"] == fn &&
      (selfId == 0 || m.j["selfId"] == selfId);
  }
  return n;
}

// whose instant cast of the proc `user`'s game was told of, in order: a
// SpellCast relay whose caster and target are that actor (the game resolves
// its own id to 0x14, another's to its figure), castingSource 3, no launch
std::vector<uint32_t> ProcCastsOn(PartOne& p, Networking::UserId user)
{
  std::vector<uint32_t> on;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::SpellCast) &&
        m.j["data"]["spell"] == kProc) {
      REQUIRE(m.j["data"]["castingSource"] == 3);
      REQUIRE(m.j["data"]["hasLaunch"] == false);
      REQUIRE(m.j["data"]["target"] == m.j["data"]["caster"]);
      on.push_back(m.j["data"]["caster"].get<uint32_t>());
    }
  }
  return on;
}

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// the proc's casts wait on the server's timer (kBondProcCastDelayMs, 500 ms):
// sleep past the delay, fire the timer (and flush whatever was deferred)
void CastsFire(PartOne& p)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(650));
  p.Tick(); // the timer
  p.Tick(); // the deferred snippets
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

TEST_CASE("A bond's claimed hit conceals both for the bond's time, cast in "
          "every game but the caster's",
          "[Bond][espm]")
{
  PartOne& p = GetPartOne();
  p.SetBondSettings(kBonds);
  auto [caster, target] = Pair(p);
  p.Messages().clear();
  const uint64_t armed = NowMs();
  Cast(p);
  Hit(p);
  p.Tick(); // snippets are deferred
  // the server's records, on both (the proc's archetype runs as a state)
  REQUIRE(caster->HasRunningEffectOfType(kInvisibility));
  REQUIRE(target->HasRunningEffectOfType(kInvisibility));
  // the casts wait for the relayed bond cast to have run in the other games:
  // read while the timer's half (250 of its 500 ms) is still ahead, since a
  // slow run can spend the whole delay before this line (the Windows build
  // under OpenCppCoverage, 2026-10-27 06:48Z, fired the proc early and read
  // a cast here: the test-clock-margins rule)
  if (NowMs() - armed < 250) {
    REQUIRE(ProcCastsOn(p, 1).empty());
  } else {
    WARN("the proc's delay passed before the wait could be read; skipped");
  }
  CastsFire(p);
  // the target's game is told of the caster's instant cast (its figure
  // there) and of its own (0x14 there); the caster's game, where the mod's
  // script cast both, hears nothing, and no Papyrus snippet carries it
  REQUIRE(ProcCastsOn(p, 1) == std::vector<uint32_t>{ kCaster, kTarget });
  REQUIRE(ProcCastsOn(p, 0).empty());
  REQUIRE(Sent(p, 0, MsgType::SpellCast) == 0);
  REQUIRE(Snippets(p, 1, "Spell", "Cast") == 0);
  REQUIRE(Snippets(p, 1, "Actor", "DispelSpell") == 0);
  Leave(p);
}

TEST_CASE("The break ends both concealments and dispels the target's own",
          "[Bond][espm]")
{
  PartOne& p = GetPartOne();
  p.SetBondSettings(kBonds);
  auto [caster, target] = Pair(p);
  Cast(p);
  Hit(p);
  CastsFire(p);
  p.Messages().clear();
  REQUIRE(p.BreakBondsOf(kCaster));
  p.Tick();
  REQUIRE_FALSE(caster->HasRunningEffectOfType(kInvisibility));
  REQUIRE_FALSE(target->HasRunningEffectOfType(kInvisibility));
  // the target's copy was the server's cast: the server dispels it; the
  // caster's is the script's own, dispelled by the script
  REQUIRE(Snippets(p, 1, "Actor", "DispelSpell", 0x14) == 1);
  REQUIRE(Snippets(p, 0, "Actor", "DispelSpell") == 0);
  // the swap as before
  REQUIRE(Sent(p, 0, MsgType::Teleport) == 1);
  REQUIRE(Sent(p, 1, MsgType::Teleport) == 1);
  Leave(p);
}

TEST_CASE("A bond left alone runs out: the concealments end and the target's "
          "is dispelled once",
          "[Bond][espm]")
{
  PartOne& p = GetPartOne();
  p.SetBondSettings(kBonds);
  auto [caster, target] = Pair(p);
  Cast(p);
  Hit(p);
  CastsFire(p);
  p.Messages().clear();
  const uint64_t now = NowMs();
  // not yet (the bonds rule's clock; the records' own seconds are the
  // effects tick's, which barely moves here)
  p.TickBonds(now + 19'000);
  p.Tick();
  REQUIRE(target->HasRunningEffectOfType(kInvisibility));
  REQUIRE(Snippets(p, 1, "Actor", "DispelSpell") == 0);
  p.TickBonds(now + 21'000);
  p.Tick();
  REQUIRE_FALSE(caster->HasRunningEffectOfType(kInvisibility));
  REQUIRE_FALSE(target->HasRunningEffectOfType(kInvisibility));
  REQUIRE(Snippets(p, 1, "Actor", "DispelSpell", 0x14) == 1);
  // once, and no swap: the bond is spent
  p.TickBonds(now + 22'000);
  p.Tick();
  REQUIRE(Snippets(p, 1, "Actor", "DispelSpell", 0x14) == 1);
  REQUIRE(Sent(p, 1, MsgType::Teleport) == 0);
  REQUIRE_FALSE(p.BreakBondsOf(kCaster));
  Leave(p);
}

TEST_CASE("A recast at the same target refreshes the concealment without a "
          "dispel",
          "[Bond][espm]")
{
  PartOne& p = GetPartOne();
  p.SetBondSettings(kBonds);
  auto [caster, target] = Pair(p);
  Cast(p);
  Hit(p);
  CastsFire(p);
  p.Messages().clear();
  Cast(p);
  Hit(p);
  CastsFire(p);
  REQUIRE(caster->HasRunningEffectOfType(kInvisibility));
  REQUIRE(target->HasRunningEffectOfType(kInvisibility));
  REQUIRE(ProcCastsOn(p, 1) == std::vector<uint32_t>{ kCaster, kTarget });
  REQUIRE(Snippets(p, 1, "Actor", "DispelSpell") == 0);
  // one bond stands: one break
  REQUIRE(p.BreakBondsOf(kTarget));
  REQUIRE_FALSE(p.BreakBondsOf(kCaster));
  Leave(p);
}

TEST_CASE("A bond broken before its casts fire casts nothing, and dispels "
          "the target's once",
          "[Bond][espm]")
{
  PartOne& p = GetPartOne();
  p.SetBondSettings(kBonds);
  auto [caster, target] = Pair(p);
  Cast(p);
  Hit(p);
  p.Tick();
  p.Messages().clear();
  REQUIRE(p.BreakBondsOf(kTarget));
  CastsFire(p);
  REQUIRE(ProcCastsOn(p, 1).empty());
  REQUIRE(Snippets(p, 1, "Actor", "DispelSpell", 0x14) == 1);
  REQUIRE_FALSE(caster->HasRunningEffectOfType(kInvisibility));
  REQUIRE_FALSE(target->HasRunningEffectOfType(kInvisibility));
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
