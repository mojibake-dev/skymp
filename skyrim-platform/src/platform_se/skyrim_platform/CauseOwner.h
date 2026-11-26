#pragma once
#include <cstdint>

// Whose engine a cause the engine names belongs to: a trigger's presser (the
// presser rule) and a physics impulse's cause (the impulse rule) share one
// rule, decided here from facts the caller reads off the reference
// (FridaHooks.cpp OwnerOf), so the rule itself needs no game.
namespace CauseRule {

enum class Owner
{
  kThisSeat,
  kOther,
  kUndecided
};

// what the caller read of an actor
struct ActorFacts
{
  uint32_t formId = 0;
  bool simulatedHere = false; // a server-made actor this seat's engine runs
  bool dead = false;
  bool hosted = false; // in the set of actors this seat simulates
  bool puppet = false; // a copy another seat simulates
  bool hasCell = false;
  bool cellHosted = false; // its parent cell is hosted here
};

enum class CauseKind
{
  kActor,  // an actor itself
  kFired,  // a projectile or an explosion
  kObject, // any other reference
};

// what the caller read of a cause
struct CauseFacts
{
  CauseKind kind = CauseKind::kObject;
  ActorFacts actor;              // kActor: the cause
  bool firer = false;            // kFired: the engine names who fired it (a
                                 // projectile's shooter, an explosion's owner)
  bool firerIsActor = false;     // kFired: that one is an actor
  ActorFacts firerFacts;         // kFired: the firing actor, when an actor
  bool cellHosted = false;       // kFired: its own cell is hosted here
  bool followedTrapBody = false; // a trap body this seat follows
};

// an actor: the player is this seat's; a server-made actor its simulator's;
// a body its cell host's; a hosted actor this seat's and a puppet
// another's; a game-file actor neither hosted nor a puppet its cell host's,
// undecided in a cell this seat does not host (no seat's word reached it)
inline Owner OwnerOfActor(const ActorFacts& a)
{
  if (a.formId == 0x14) {
    return Owner::kThisSeat;
  }
  if (a.formId >= 0xff000000) {
    return a.simulatedHere ? Owner::kThisSeat : Owner::kOther;
  }
  if (a.dead) {
    return a.cellHosted ? Owner::kThisSeat : Owner::kOther;
  }
  if (a.hosted || a.puppet) {
    return a.hosted && !a.puppet ? Owner::kThisSeat : Owner::kOther;
  }
  if (!a.hasCell) {
    return Owner::kOther;
  }
  return a.cellHosted ? Owner::kThisSeat : Owner::kUndecided;
}

// a cause: an actor by the actor's rule; any other reference this seat's,
// but a trap body another seat's that this seat follows
inline Owner OwnerOfCause(const CauseFacts& c)
{
  if (c.kind == CauseKind::kActor) {
    return OwnerOfActor(c.actor);
  }
  return c.followedTrapBody ? Owner::kOther : Owner::kThisSeat;
}

}
