#pragma once

// Where a script's PushActorAway runs, by who pushes whom (the push-figure
// hook, ScriptNatives.cpp). A player's body is its pusher's for the push's
// throw, as it is its gripper's for a grip: the pusher's own game throws its
// copy of the pushed player and claims the body for the flight, and the
// pushed player's own game follows that stream until the body's rest hands
// it back. So the push onto another player's figure runs on the game whose
// own player pushes, and is skipped on every other game (the pushed player's
// own, and a bystander's, where the pusher is a figure too); the push onto
// this game's own player by another player's figure is skipped, since the
// pusher's game throws it; a push onto a puppet (an actor another seat
// simulates) is skipped and reported, its host throwing it; anything else
// runs here.
namespace PushRoute {

enum class Route
{
  kRun,
  kSkip,
  kSkipAndReport
};

struct Facts
{
  // the frame's self is this game's player
  bool pusherIsOwnPlayer = false;
  // the frame's self is another player's figure
  bool pusherIsPlayerFigure = false;
  // the target is this game's player
  bool targetIsOwnPlayer = false;
  // the target is another player's figure
  bool targetIsPlayerFigure = false;
  // the target is an actor another seat simulates (a puppet)
  bool targetIsPuppet = false;
};

inline Route Of(const Facts& f)
{
  if (f.targetIsPlayerFigure) {
    return f.pusherIsOwnPlayer ? Route::kRun : Route::kSkip;
  }
  if (f.targetIsOwnPlayer) {
    return f.pusherIsPlayerFigure ? Route::kSkip : Route::kRun;
  }
  if (f.targetIsPuppet) {
    return Route::kSkipAndReport;
  }
  return Route::kRun;
}

}
