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
// one beside it still does; one gone beyond its reach did already. The
// players stand where the loaded lever stands, never a typed position.
namespace {
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kLever = 0xaa8c3;
constexpr uint32_t kPlayerActor = 0xff000fd1;
constexpr uint32_t kNpcActor = 0xff000fd2;

struct LeverStage
{
  PartOne& p;
  MpObjectReference& lever;
  MpActor& player;
  MpActor& npc;
};

// the lever loaded, the player (user 0) and a user-less NPC beside it, the
// NPC holding it by a host's report, as the scene's bandit did
LeverStage NpcTakesTheLever()
{
  auto& p = GetPartOne();
  auto& lever = p.worldState.GetFormAt<MpObjectReference>(kLever);
  const auto base =
    p.worldState.GetEspm().GetBrowser().LookupById(lever.GetBaseId());
  REQUIRE(base.rec);
  REQUIRE(base.rec->GetType() == "FURN");

  const NiPoint3 at = lever.GetPos();
  DoConnect(p, 0);
  p.CreateActor(kPlayerActor, at + NiPoint3{ 60.f, 0.f, 0.f }, 0, kTemple);
  p.SetUserActor(0, kPlayerActor);
  p.CreateActor(kNpcActor, at + NiPoint3{ 0.f, 40.f, 0.f }, 0, kTemple);
  auto& player = p.worldState.GetFormAt<MpActor>(kPlayerActor);
  auto& npc = p.worldState.GetFormAt<MpActor>(kNpcActor);

  constexpr bool kDefaultProcessingOnly = false;
  constexpr bool kSecondActivation = false;
  constexpr bool kReportedByHost = true;
  lever.Activate(npc, kDefaultProcessingOnly, kSecondActivation,
                 kReportedByHost);
  return { p, lever, player, npc };
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
  s.p.DestroyActor(kNpcActor);
}
}

TEST_CASE("A furniture's dead occupant holds it no more: a player's pull of "
          "Bleak Falls Temple's puzzle lever is answered with its use",
          "[PartOne][espm]")
{
  auto s = NpcTakesTheLever();

  s.npc.Kill(nullptr);
  REQUIRE(s.npc.IsDead());

  s.p.Messages().clear();
  s.lever.Activate(s.player);
  REQUIRE(UseSentTo(s.p, 0, kLever));

  Leave(s);
}

TEST_CASE("A furniture's live occupant beside it still holds it: a player's "
          "pull is refused its use",
          "[PartOne][espm]")
{
  auto s = NpcTakesTheLever();

  s.p.Messages().clear();
  s.lever.Activate(s.player);
  REQUIRE_FALSE(UseSentTo(s.p, 0, kLever));

  // a second try is refused the same way: the NPC still holds it
  s.p.Messages().clear();
  s.lever.Activate(s.player);
  REQUIRE_FALSE(UseSentTo(s.p, 0, kLever));

  Leave(s);
}

TEST_CASE("A furniture's occupant gone beyond its reach holds it no more",
          "[PartOne][espm]")
{
  auto s = NpcTakesTheLever();

  // past the furniture's 256-unit occupation reach
  s.npc.SetPos(s.lever.GetPos() + NiPoint3{ 300.f, 0.f, 0.f });

  s.p.Messages().clear();
  s.lever.Activate(s.player);
  REQUIRE(UseSentTo(s.p, 0, kLever));

  Leave(s);
}
