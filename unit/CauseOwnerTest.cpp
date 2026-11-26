#include "../skyrim-platform/src/platform_se/skyrim_platform/CauseOwner.h"
#include <catch2/catch_all.hpp>

// The cause owner rule (CauseOwner.h), shared by the presser rule and the
// impulse rule (skyrim-platform FridaHooks.cpp OwnerOf): whose engine a
// trigger's presser or an impulse's cause belongs to. One engine owns each
// push and each press: a projectile or an explosion is its firing actor's,
// one no actor fired its cell host's.
namespace {
using CauseRule::ActorFacts;
using CauseRule::CauseFacts;
using CauseRule::CauseKind;
using CauseRule::Owner;
using CauseRule::OwnerOfCause;

ActorFacts Player()
{
  ActorFacts a;
  a.formId = 0x14;
  a.hasCell = true;
  return a;
}

// another player's figure on this seat: a server-made actor another seat runs
ActorFacts Figure()
{
  ActorFacts a;
  a.formId = 0xff000801;
  a.hasCell = true;
  return a;
}

CauseFacts FiredBy(const ActorFacts& firer)
{
  CauseFacts c;
  c.kind = CauseKind::kFired;
  c.firer = true;
  c.firerIsActor = true;
  c.firerFacts = firer;
  return c;
}

// fired by no actor: none named, or a reference that is no actor (a
// destructible's own explosion names the object itself)
CauseFacts FiredByNoActor(bool named, bool cellHosted)
{
  CauseFacts c;
  c.kind = CauseKind::kFired;
  c.firer = named;
  c.cellHosted = cellHosted;
  return c;
}
}

TEST_CASE("A projectile another player's figure fired is another seat's, so "
          "its press and its push are that player's, and one this seat's "
          "player fired is this seat's",
          "[CauseOwner]")
{
  REQUIRE(OwnerOfCause(FiredBy(Figure())) == Owner::kOther);
  REQUIRE(OwnerOfCause(FiredBy(Player())) == Owner::kThisSeat);
  // an NPC this seat simulates fires for this seat, a puppet for its own
  ActorFacts hosted;
  hosted.formId = 0x9bcd6;
  hosted.hosted = true;
  hosted.hasCell = true;
  REQUIRE(OwnerOfCause(FiredBy(hosted)) == Owner::kThisSeat);
  ActorFacts puppet = hosted;
  puppet.hosted = false;
  puppet.puppet = true;
  REQUIRE(OwnerOfCause(FiredBy(puppet)) == Owner::kOther);
  // an actor itself keeps its owner: the player, a figure, a body (its cell
  // host's), an NPC no seat's word reached (undecided where the cell is not
  // hosted here)
  const auto actor = [](const ActorFacts& a) {
    CauseFacts c;
    c.kind = CauseKind::kActor;
    c.actor = a;
    return OwnerOfCause(c);
  };
  REQUIRE(actor(Player()) == Owner::kThisSeat);
  REQUIRE(actor(Figure()) == Owner::kOther);
  ActorFacts body;
  body.formId = 0x9bcd6;
  body.dead = true;
  body.hasCell = true;
  body.cellHosted = true;
  REQUIRE(actor(body) == Owner::kThisSeat);
  body.cellHosted = false;
  REQUIRE(actor(body) == Owner::kOther);
  ActorFacts stranger;
  stranger.formId = 0x380ba;
  stranger.hasCell = true;
  REQUIRE(actor(stranger) == Owner::kUndecided);
  stranger.cellHosted = true;
  REQUIRE(actor(stranger) == Owner::kThisSeat);
}

TEST_CASE("An explosion or a projectile no actor fired is its cell host's: "
          "a destructible's own explosion, a trap's dart",
          "[CauseOwner]")
{
  // the object named as its own explosion's owner, in a cell hosted here
  REQUIRE(OwnerOfCause(FiredByNoActor(true, true)) == Owner::kThisSeat);
  // the same on a seat that does not host the cell: no claim of its own
  REQUIRE(OwnerOfCause(FiredByNoActor(true, false)) == Owner::kUndecided);
  // none named (a dart, a caster activator)
  REQUIRE(OwnerOfCause(FiredByNoActor(false, true)) == Owner::kThisSeat);
  REQUIRE(OwnerOfCause(FiredByNoActor(false, false)) == Owner::kUndecided);
  // any other reference keeps its owner: a prop this seat's, a trap body
  // another seat runs that this seat follows another's
  CauseFacts prop;
  REQUIRE(OwnerOfCause(prop) == Owner::kThisSeat);
  prop.followedTrapBody = true;
  REQUIRE(OwnerOfCause(prop) == Owner::kOther);
}
