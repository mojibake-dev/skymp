#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "AnimationData.h"
#include "HitMessage.h"

PartOne& GetPartOne();
using namespace std::chrono_literals;

// thuum docs/verbs/stagger.md: whether a hit staggers its victim, and how
// hard, is the server's (wire-rules stagger) by the engine's formula (thuum
// ghidra/notes/block-formula.md): the attacker's weapon's stagger times the
// attack's stagger offset over the victim's mass, and the victim's game is
// told to play it (TESModPlatform.Stagger by SpSnippet, with the attacker
// as the source). Skyrim.esm (thuum lab/esm.py, 2026-10-10): the Iron Sword
// 0x12EB7 (stagger 0.75), the Iron Shield 0x12EB6 (rating 20), NordRace
// (the player's: Mass 1, attackStart 0, attackPowerStartForward 1,
// bashStart 0.5), fStaggerPlayerMassMult 2.5, fStaggerMassOffsetBase 2.1.
// The test's damage formula deals 25 for any hit, against the victim's
// recorded 250.
namespace {
constexpr uint32_t kAggressor = 0xff000000;
constexpr uint32_t kVictim = 0xff000001;
constexpr uint32_t kIronSword = 0x00012eb7;
constexpr uint32_t kIronShield = 0x00012eb6;

Inventory::ExtraData Worn(bool left)
{
  Inventory::ExtraData extra;
  if (left) {
    extra.wornLeft = true;
  } else {
    extra.worn_ = true;
  }
  return extra;
}

// The aggressor (user 0) at the origin facing north with an Iron Sword; the
// victim (user 1) 100 units north facing it, its health 250 and its Block
// 15 recorded, an Iron Shield in its left hand when `shield`
MpActor& Pair(PartOne& p, bool shield)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kAggressor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kAggressor);
  p.CreateActor(kVictim, { 0, 100, 0 }, 0, 0x3c);
  p.SetUserActor(1, kVictim);
  auto& aggressor = p.worldState.GetFormAt<MpActor>(kAggressor);
  aggressor.AddItem(kIronSword, 1);
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(kIronSword, 1, Worn(false)));
  aggressor.SetEquipment(eq);

  auto& victim = p.worldState.GetFormAt<MpActor>(kVictim);
  victim.SetAngle({ 0.f, 0.f, 180.f });
  ActorValueRecord record;
  record.bases = { { 9, 15.f }, { 24, 250.f } };
  REQUIRE(victim.SetActorValueRecord(record));
  if (shield) {
    victim.AddItem(kIronShield, 1);
    Equipment veq;
    veq.inv.entries.push_back(Inventory::Entry(kIronShield, 1, Worn(true)));
    victim.SetEquipment(veq);
  }
  return victim;
}

// the aggressor's game starts an attack (its animation event)
void Start(PartOne& p, const char* event)
{
  AnimationData anim;
  anim.animEventName = event;
  p.animationSystem.Process(&p.worldState.GetFormAt<MpActor>(kAggressor),
                            anim);
}

// a sword hit as the aggressor's game reports it
void Hit(PartOne& p, bool power, bool bash)
{
  p.Messages().clear();
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kVictim;
  hit.data.source = kIronSword;
  hit.data.isPowerAttack = power;
  hit.data.isBashAttack = bash;
  p.GetActionListener().OnHit(raw, hit);
}

// the magnitudes of the staggers the victim's game was told to play, each
// with the player as the target and the aggressor as the source
std::vector<double> Staggers(PartOne& p)
{
  p.Tick(); // snippets are deferred
  std::vector<double> out;
  for (auto& m : p.Messages()) {
    if (m.userId != 1 || m.j["t"] != MsgType::SpSnippet ||
        m.j["class"] != "TESModPlatform" || m.j["function"] != "Stagger") {
      continue;
    }
    const auto& args = m.j["arguments"];
    REQUIRE(args.size() == 3);
    REQUIRE(args[0]["formId"] == 0x14);
    REQUIRE(args[2]["formId"] == kAggressor);
    out.push_back(args[1].get<double>());
  }
  return out;
}

void Leave(PartOne& p)
{
  p.DestroyActor(kVictim);
  p.DestroyActor(kAggressor);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A power attack staggers a player by the engine's formula",
          "[Stagger][espm]")
{
  PartOne& p = GetPartOne();
  Pair(p, false);
  Start(p, "attackPowerStartForward");
  Hit(p, true, false);
  // 0.75 x 1 / (1 x 1 x 2.5 + 0) - (2.5 - 2.1) x 0.15 = 0.24
  const auto staggers = Staggers(p);
  REQUIRE(staggers.size() == 1);
  REQUIRE(staggers[0] == Catch::Approx(0.24).margin(1e-5));
  Leave(p);
}

TEST_CASE("A plain swing staggers nobody", "[Stagger][espm]")
{
  PartOne& p = GetPartOne();
  Pair(p, false);
  Start(p, "attackStart");
  Hit(p, false, false);
  REQUIRE(Staggers(p).empty());
  Leave(p);
}

TEST_CASE("A blocked power attack staggers less, and always",
          "[Stagger][espm]")
{
  PartOne& p = GetPartOne();
  auto& victim = Pair(p, true);
  victim.SetIsBlockActive(true);
  Start(p, "attackPowerStartForward");
  Hit(p, true, false);
  // less 0.001 x 20 + 0.01 + 0.005 x 15 + 0.001 + max(0, (1 - 2.1) x 0.15)
  // = 0.106: 0.134
  const auto staggers = Staggers(p);
  REQUIRE(staggers.size() == 1);
  REQUIRE(staggers[0] == Catch::Approx(0.134).margin(1e-5));
  Leave(p);
}

TEST_CASE("A bash staggers a player who is not blocking and not one who is",
          "[Stagger][espm]")
{
  {
    PartOne& p = GetPartOne();
    Pair(p, false);
    Start(p, "bashStart");
    Hit(p, false, true);
    // the sword's bash at the attacker's unrecorded Block (0):
    // (0 x 0.01 + 0.5) / (1 x 1 + 0) x 0.2 x 0.5 = 0.05
    const auto staggers = Staggers(p);
    REQUIRE(staggers.size() == 1);
    REQUIRE(staggers[0] == Catch::Approx(0.05).margin(1e-5));
    Leave(p);
  }
  {
    PartOne& p = GetPartOne();
    auto& victim = Pair(p, true);
    victim.SetIsBlockActive(true);
    Start(p, "bashStart");
    Hit(p, false, true);
    // times fStaggerBlockingMult, 0
    REQUIRE(Staggers(p).empty());
    Leave(p);
  }
}
