#include "Messages.h"
#include "TestUtils.hpp"
#include <algorithm>
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum playtest 47 (2026-11-06): Bleak Falls Temple 0x371de's puzzle lever
// 0xaa8c3 (FURN 0xb7db4 LeverPushAnimatingNoPlayer) never animated for a
// player after its scene's bandit 0x39fe1 pulled it and the darts killed him
// beside it: the server kept him as the furniture's occupant, since a host's
// NPC takes a furniture by its reported activation and nothing reports its
// leaving, so every player's pull was refused ("occupant is another object
// and is nearby") and its OpenContainer reply, which makes the player's game
// use the furniture, never came. A dead occupant holds it no more; a live
// one beside it still does; one gone beyond its reach did already; a
// deleted one (a runtime 0xff actor, the only kind Delete marks) holds it no
// more either. The occupant is the scene's own bandit, loaded from the game
// files as ActivateParentTest loads 0x39fe4, so it is an NPC and not a
// user-less created actor (which reads as a player's, IsCreatedAsPlayer).
// The players stand where the loaded lever stands, never a typed position.
namespace {
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kLever = 0xaa8c3;
constexpr uint32_t kBandit = 0x39fe1;
constexpr uint32_t kPlayerActor = 0xff000fd1;
constexpr uint32_t kRuntimeActor = 0xff000fd2;

struct LeverStage
{
  PartOne& p;
  MpObjectReference& lever;
  MpActor& player;
  MpActor& occupant;
};

// the lever loaded, the player (user 0) beside it, and the occupant taking
// the lever by a host's report, as the scene's bandit did
LeverStage TakeTheLever(PartOne& p, MpActor& occupant)
{
  auto& lever = p.worldState.GetFormAt<MpObjectReference>(kLever);
  const auto base =
    p.worldState.GetEspm().GetBrowser().LookupById(lever.GetBaseId());
  REQUIRE(base.rec);
  REQUIRE(base.rec->GetType() == "FURN");

  const NiPoint3 at = lever.GetPos();
  occupant.SetPos(at + NiPoint3{ 0.f, 40.f, 0.f });
  DoConnect(p, 0);
  p.CreateActor(kPlayerActor, at + NiPoint3{ 60.f, 0.f, 0.f }, 0, kTemple);
  p.SetUserActor(0, kPlayerActor);
  auto& player = p.worldState.GetFormAt<MpActor>(kPlayerActor);

  constexpr bool kDefaultProcessingOnly = false;
  constexpr bool kSecondActivation = false;
  constexpr bool kReportedByHost = true;
  lever.Activate(occupant, kDefaultProcessingOnly, kSecondActivation,
                 kReportedByHost);
  return { p, lever, player, occupant };
}

// the scene's bandit as the occupant, loaded from the game files
LeverStage BanditTakesTheLever()
{
  auto& p = GetPartOne();
  p.worldState.npcSettings["Skyrim.esm"].spawnInInterior = true;
  p.worldState.npcEnabled = true;
  auto& bandit = p.worldState.GetFormAt<MpActor>(kBandit);
  return TakeTheLever(p, bandit);
}

// the reply that makes the player's game use the furniture
bool UseSentTo(PartOne& p, Networking::UserId user, uint32_t target)
{
  return std::any_of(
    p.Messages().begin(), p.Messages().end(), [&](PartOne::Message& m) {
      return m.userId == user && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::OpenContainer) &&
        m.j.contains("target") && m.j["target"] == target;
    });
}

void Leave(LeverStage& s)
{
  DoDisconnect(s.p, 0);
  s.p.DestroyActor(kPlayerActor);
}
}

TEST_CASE("A furniture's dead occupant holds it no more: a player's pull of "
          "Bleak Falls Temple's puzzle lever is answered with its use",
          "[PartOne][espm]")
{
  auto s = BanditTakesTheLever();

  s.occupant.Kill(nullptr);
  REQUIRE(s.occupant.IsDead());

  s.p.Messages().clear();
  s.lever.Activate(s.player);
  REQUIRE(UseSentTo(s.p, 0, kLever));

  Leave(s);
}

TEST_CASE("A furniture's live occupant beside it still holds it: a player's "
          "pull is refused its use",
          "[PartOne][espm]")
{
  auto s = BanditTakesTheLever();

  s.p.Messages().clear();
  s.lever.Activate(s.player);
  REQUIRE_FALSE(UseSentTo(s.p, 0, kLever));

  // a second try is refused the same way: the bandit still holds it
  s.p.Messages().clear();
  s.lever.Activate(s.player);
  REQUIRE_FALSE(UseSentTo(s.p, 0, kLever));

  Leave(s);
}

TEST_CASE("A furniture's occupant gone beyond its reach holds it no more",
          "[PartOne][espm]")
{
  auto s = BanditTakesTheLever();

  // past the furniture's 256-unit occupation reach
  s.occupant.SetPos(s.lever.GetPos() + NiPoint3{ 300.f, 0.f, 0.f });

  s.p.Messages().clear();
  s.lever.Activate(s.player);
  REQUIRE(UseSentTo(s.p, 0, kLever));

  Leave(s);
}

TEST_CASE("A furniture's deleted occupant holds it no more", "[PartOne][espm]")
{
  // Delete marks only a runtime 0xff form (a game-file one is refused with a
  // warning, MpObjectReference::Delete), so this occupant is created
  auto& p = GetPartOne();
  auto& lever = p.worldState.GetFormAt<MpObjectReference>(kLever);
  p.CreateActor(kRuntimeActor, lever.GetPos(), 0, kTemple);
  auto& runtimeActor = p.worldState.GetFormAt<MpActor>(kRuntimeActor);
  auto s = TakeTheLever(p, runtimeActor);

  s.occupant.Delete();
  REQUIRE(s.occupant.IsDeleted());

  s.p.Messages().clear();
  s.lever.Activate(s.player);
  REQUIRE(UseSentTo(s.p, 0, kLever));

  // the deleted actor stays in the world state; GetPartOne() is fresh per
  // test, so it is not destroyed here
  Leave(s);
}
