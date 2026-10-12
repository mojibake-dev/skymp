#pragma once
#include "MpActor.h"
#include <cstdint>

// thuum docs/verbs/shouts.md: the player's own game told what the server's
// record holds, as the engine keeps it: a shout in its list
// (Actor.AddShout), a word known (Game.TeachWord), a word unlocked
// (Game.UnlockWord), each by SpSnippet. Teaching again what the game knows
// changes nothing there.
void SendShoutEntry(MpActor& actor, MpActor::ShoutEntry kind, uint32_t formId);

// The whole record, after a login (shouts, then words known, then words
// unlocked, as the engine needs a word known before it unlocks it)
void SendShoutRecord(MpActor& actor);
