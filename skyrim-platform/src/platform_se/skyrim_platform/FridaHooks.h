#pragma once

#include <cstdint>

namespace Frida {

void InstallHooks();

// thuum docs/verbs/cast-action.md: the hand whose cast start the engine's
// action entry performed for the player and no cast event has claimed yet
// (0 left, 1 right; the perk's dual cast is the left hand's actions), the
// oldest within withinMs, taken (cleared) by the call, or -1. The cast event
// names no hand, and the casters' states could not tell them apart when both
// held the spell (x-left-firebolt2 20261015-070447), where the action does;
// one pending start per hand keeps two hands casting the same spell apart
// (playtest nineteen: the latest start's hand named both casts, so the
// figure streamed from one hand twice). A hand's Interrupt or Equipped
// action drops its pending start (a cancelled charge fires no cast event).
int32_t TakeCastStart(uint64_t withinMs);

// thuum docs/verbs/cast-action.md: whether the actor's graph has answered a
// cast request for the hand (0 left, 1 right, 2 voice) since the last take:
// the BeginCastLeft, BeginCastRight or BeginCastVoice graph event reaching
// its handler, which is when the engine's own caster performs the Attack
// action. Taken (cleared) by the call. The view performs a figure's start on
// this answer, as the engine does, not on a timer (x-cast-gap
// 20261015-213755: every taken start 4 to 7 ms after the answer; moving
// figures answered later than the view's one update, playtest nineteen).
bool TakeCastReady(uint32_t actorFormId, int32_t hand);

// records an answer (hand bit 1 left, 2 right, 4 voice); called by the
// typed hooks on the response handlers (Hooks.cpp)
void NoteCastReady(uint32_t actorFormId, uint32_t handBit);

// thuum docs/verbs/marksman.md: open on this thread while TESModPlatform's
// LaunchArrow or LaunchArrowAt launches the arrow of a shot the server
// relayed. A figure wears its quiver (skymp5-client equipment.ts), so its bow
// releasing in this game launches an arrow of its own beside the relayed one
// (x-double-arrow2 20261012-022726: two launches for one shot); the launch
// hook lets a figure's arrow through only inside one of these.
struct RelayedArrowScope
{
  RelayedArrowScope();
  ~RelayedArrowScope();
  RelayedArrowScope(const RelayedArrowScope&) = delete;
  RelayedArrowScope& operator=(const RelayedArrowScope&) = delete;
};
}
