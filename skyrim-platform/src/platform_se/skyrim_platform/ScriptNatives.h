#pragma once
#include <cstdint>

namespace RE::BSScript {
class IFunction;
}

// thuum docs/DECISIONS.md ADR-068 (scripts as one system; rung 2, the one
// seam, docs/research/scripts-one-system-2026-11-05.md 6.2): the VM's one
// native dispatch, BSScript::NF_util::NativeFunctionBase::Call
// (CommonLibSSE-NG include/RE/N/NativeFunctionBase.h:47, IFunction's slot
// 0x0F; Address Library 104651, game/Offsets.h NativeFunctionCall), hooked
// once for every tabled native (NativeRows.inc, thuum docs/NATIVES.md's
// rows): the call judged from its typed frame, the subject's owner tested
// (a world object's cell host, an actor's simulator, the player's own seat;
// a trap family reference every seat's own, ADR-068 D-S3, TrapFamily.h),
// an owned action row's call (and a script's Kill, a trap's ProcessTrapHit
// on an actor this seat simulates, and a family reference's SetDestroyed,
// SetMotionType and ApplyHavokImpulse; since rung 3 an owned state row's
// call as a touch, the script record's trigger; since rung 4, the state
// rows, an owned record row's call with its arguments, RefState's and the
// standing's, and on every seat an alias row's, ForceRefTo's and Clear's,
// with the alias's quest and id) queued into this frame's batch and raised
// to JS once a frame as the event "scriptCalls", each with its caller's
// self, every other tabled call counted and raised every 30 s as
// "scriptCensus"; the hook is a replacement, so a lever (scriptNonOwnerSkip:
// none by default, nonLatent, or rung 4's recorded) can skip a non-owned
// state row's body. The rows' functions are resolved at
// kDataLoaded from the VM's own bindings (Hooks.cpp BindNativeMethod's
// record, by class and name), never from a pinned offset (rule 1). This
// replaced the flip's 24 entry hooks (ADR-062 decision 2) and their per-call
// "scriptNative" event.
namespace ScriptNatives {
// at kDataLoaded: the table's rows bound from the VM's bindings
// (Hooks::GetBoundNatives) and the lever read, then the VM's one dispatch
// replaced once both are complete, where every tabled call is judged from
// its frame and the platform's latent calls are marked (PlatformCall.h)
void InstallHooks();
// every frame from the platform's OnUpdate: the census flushed on time (a
// quiet seat emits its last window; the review's medium finding)
void Tick();
// thuum docs/verbs/npc-fate.md (playtest 48): this frame's batch, when it
// holds a call, raised on the update's task queue now, ahead of the task the
// caller adds next: a death event's (EventHandler.cpp), so the script's call
// behind the death (a trap's ProcessTrapHit on the body, a script's Kill)
// reaches JS before deathStart does, in the same update (Viet::TaskQueue
// runs its tasks in the order they were added), and the client sends the
// hit's evidence ahead of its death report. Without it the batch rides the
// tick's queue, and whether a tick runs between the hit and the update that
// carries deathStart is the frame's order, unread (HYPOTHESIS either way)
void FlushAheadOnUpdate();
// inside a script action: true on this thread for the extent of an owned
// action row's call (a script's PlayAnimation on a world object in a cell
// this game hosts), the mark PlatformCall's bracket keeps, so the
// engine-level animation hooks (FridaHooks.cpp: SendAnimationEvent 38048,
// PlayAnimation_Impl 14297) leave the animation the boundary reports
// (thuum ADR-068 decision 7, docs/research/scripts-one-system-2026-11-05.md
// 6.7): a script's animation is reported once
bool InScriptAction();
// rung 3's switch on the touch (thuum ADR-068, the script record; thuum
// .claude/rules/native.md's kill switch): setScriptRecordTouches(enabled)
// (PuppetApi.cpp) stores one atomic flag, on by default so a client that
// never calls it keeps the batch it has; QueueTouch reads it first, so with
// it off no owned state row's call is read for its caller or queued as a
// touch (the census still counts the call)
void SetTouchesEnabled(bool enabled);
bool TouchesEnabled();
// the platform's direct call of a native (SkyrimPlatform.cpp's worker, under
// PlatformCall::Scope), counted in the census as "(platform)" when the
// native is tabled: made inside the OnUpdate native's own Call, it never
// reaches the replacement (Gum runs a call nested in a replaced call's
// original straight to the original)
void CountPlatformCall(const RE::BSScript::IFunction* fn);
}
