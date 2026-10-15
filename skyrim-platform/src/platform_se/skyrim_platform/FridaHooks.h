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

}
