#pragma once

#include <cstdint>

namespace Frida {

void InstallHooks();

// thuum docs/verbs/cast-action.md: the hand of the player's last cast action
// (0 left, 1 right; a dual cast is the left hand's actions) as the engine's
// action entry performed it within the last withinMs, or -1. The cast event
// names no hand, and the casters' states could not tell them apart when both
// held the spell (x-left-firebolt2 20261015-070447), where the action did.
int32_t LastCastHand(uint64_t withinMs);

}
