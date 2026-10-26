#pragma once

#include <cstddef>
#include <cstdint>

// thuum docs/verbs/npc-stream.md (ADR-050): the actors this game hosts, by
// form id, beside the puppets (Puppets.h): the server's word per actor as
// skymp5-client's HostService mirrors it (setHosted at every flip of the
// word). Read by the hook on the engine's action entry (FridaHooks.cpp,
// ActorMediator::PerformAction) on the game thread: a hosted actor's actions
// are reported as the player's are and travel as UpdateAction under the
// actor's own id, and the puppets perform them through the same entry. A
// mutex-guarded set, Puppets.h's idiom; written on the JS thread, no game
// call.
namespace Hosted {
bool Is(uint32_t formId);
void Set(uint32_t formId, bool hosted);
size_t Count();

// The actors inside the engine's action entry right now, a depth per form
// id (the mediator re-enters itself for a paired action's target), marked by
// the hook from the entry to the leave for the player and the hosted
// actors. While the mark is on, every event the engine sends the actor's
// graph is the action's own; skymp5-client's relay source (sync/animation.ts)
// reads it through the isPerformingAction export inside the
// sendAnimationEvent hook, where the game thread waits (Hook.cpp's
// PushAndWait), and drops the event: the action travels instead, and a
// puppet plays it once.
void BeginPerforming(uint32_t formId);
void EndPerforming(uint32_t formId);
bool IsPerforming(uint32_t formId);
}
