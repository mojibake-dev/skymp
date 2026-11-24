#pragma once

// Where a script's PushActorAway runs, by who pushes whom (the push-figure
// hook, ScriptNatives.cpp). A body a player pushes is its pusher's for the
// throw, as it is its gripper's for a grip, a player's body and an NPC's
// alike: the pusher's own game throws its copy and claims the body for the
// flight, and every other game (the pushed player's own, the NPC's host, a
// bystander's) follows that stream until the body's rest hands it back. So a
// push by this game's own player runs here, and a push by another player's
// figure is skipped. An NPC's push onto another player's figure is skipped
// too, that player's own game throwing it; an NPC's push onto a puppet (an
// actor another seat simulates) is skipped and reported, its host throwing
// it; anything else runs here.
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
  if (f.pusherIsOwnPlayer) {
    return Route::kRun;
  }
  if (f.pusherIsPlayerFigure) {
    return Route::kSkip;
  }
  if (f.targetIsPlayerFigure) {
    return Route::kSkip;
  }
  if (f.targetIsPuppet) {
    return Route::kSkipAndReport;
  }
  return Route::kRun;
}

}
