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
// "scriptNative" event. Since rung 5 (thuum docs/research/
// rung5-quests-2026-11-09.md 1(c); ActingScope.h) the hook reads first, for
// every native the game calls, whether its stack is scoped to another
// player's relayed stage, start, stop or trigger entry (one relaxed load
// while no scope exists): on a scoped stack the player's own actor in the
// native's frame is that player's figure, Game.GetPlayer answers the figure,
// and a stage, a start or a stop it sets carries the scope on, each swap
// counted " (acting)".
namespace ScriptNatives {
// at kDataLoaded: the table's rows bound from the VM's bindings
// (Hooks::GetBoundNatives) and the lever read, then the VM's one dispatch
// replaced once both are complete, where every tabled call is judged from
// its frame and the platform's latent calls are marked (PlatformCall.h)
void InstallHooks();
// every frame from the platform's OnUpdate: the census flushed on time (a
// quiet seat emits its last window; the review's medium finding), and the
// acting scope's expired entries dropped (rung 5), and since train 52 the
// effect rule's expired helpers
void Tick();
// at a game load (PapyrusTESModPlatform.cpp's TESLoadGameEvent sink, beside
// the other tables that name the old world): the seam's own such tables
// cleared, train 52's effect helpers (Fix 2: a helper's id names the old
// world's reference) and its parked leveled AddItems (Fix 1: a stack id names
// the old world's stack). No game call; threadsafe
void ClearAtLoad();
// thuum train 52, Fix 1 (D-R1 C's leveled gap; the plan's section 1.2: park,
// note, close): a script's AddItem of a leveled list on the seat's own player
// is parked at its call by its stack id (OnCall), and the window an
// AddItemFunctor's operator() opens on its own thread for its stack
// (Hooks.cpp's thunk; CommonLibSSE-NG include/RE/D/DelayFunctor.h:35, slot
// 01, and :43, stackID) names the container events inside it as that
// stack's roll (NoteContainerChange); its close queues the parked call into
// the batch with the bases that landed (`landed`). The thread's previous
// window comes back at the close, so a nested functor on one thread is safe.
// Observe only: suppresses nothing. Cost: one thread_local write and read
// per AddItemFunctor in the game, and a map lookup only while a call is
// parked. HYPOTHESIS H-LR3: the functor's operator() and the events inside
// it run on one OS thread with no fiber switch between them
// (x-additem-functor 20261110-151634: its thread ids)
struct FunctorWindow
{
  explicit FunctorWindow(std::uint32_t stackId);
  ~FunctorWindow();
  FunctorWindow(const FunctorWindow&) = delete;
  FunctorWindow& operator=(const FunctorWindow&) = delete;

private:
  std::uint32_t stackId_ = 0;
  std::uint32_t saved_ = 0;
};
// a container event's fields (EventHandler.cpp's TESContainerChangedEvent
// sink, on the event's own thread; CommonLibSSE-NG
// include/RE/T/TESContainerChangedEvent.h:11-14): noted onto the parked
// call of the thread's open window when it is a new item (no old container)
// in that call's self, a base, a positive count. One thread_local read on
// every container event in the game outside a window (rule 9)
void NoteContainerChange(std::uint32_t oldContainer,
                         std::uint32_t newContainer, std::uint32_t base,
                         std::int32_t count);
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
// thuum docs/DECISIONS.md ADR-068 D-W2 A (Eli 2026-11-07; docs/research/
// fifty-fixes-2026-11-07.md section 7.2(a)): the look's switch,
// setTriggerLook(on) (PuppetApi.cpp), one atomic flag, off by default, so a
// client that never sets it keeps D-W1's trigger calls on its own seat; it
// acts only with the presser rule on (Frida::TriggerPresserRuleOn). With both
// on, a trap family trigger's own call on itself inside its firing window
// (TrapFamily::Firing, the presser rule's note) is queued as kind "look" and
// still runs here (OnFamilyCall)
void SetTriggerLook(bool on);
bool TriggerLook();
// the platform's direct call of a native (SkyrimPlatform.cpp's worker, under
// PlatformCall::Scope), counted in the census as "(platform)" when the
// native is tabled: made inside the OnUpdate native's own Call, it never
// reaches the replacement (Gum runs a call nested in a replaced call's
// original straight to the original)
void CountPlatformCall(const RE::BSScript::IFunction* fn);
}
