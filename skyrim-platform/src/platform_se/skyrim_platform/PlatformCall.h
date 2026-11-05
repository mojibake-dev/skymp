#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_set>

// thuum docs/DECISIONS.md ADR-062 decision 2 (slice 1b's reviews, 2026-11-03;
// the flip's native lane, docs/verbs/papyrus-events.md) and ADR-068 (rung
// 2's one seam): the platform's own native calls reach the same dispatch the
// seam judges (ScriptNatives.cpp, NativeFunctionBase::Call), so the seam
// must tell the platform's call from a script frame's. One mark, a
// thread-local depth that Active() reads, raised on two paths:
// - the direct path (callNative's worker inside OnUpdate, every non-latent
//   SpSnippet, the client applying the server's word through setOpen, lock,
//   activate, kill, setValue): the native body runs inside fPtr->Call on the
//   calling thread, so Scope brackets the call (SkyrimPlatform.cpp);
// - the latent path (VmCall::Run through DispatchMethodCall2 or
//   DispatchStaticCall): the VM only enqueues the call and runs the body
//   later on a Papyrus thread under a new stack id, inside
//   NativeFunctionBase::Call with the dispatched Stack as the argument, and
//   that Stack's callback is the VmCallback the platform handed the dispatch.
//   The live callbacks are kept here (NoteCallback at the dispatch,
//   ForgetCallback when the callback runs or dies), and the hook on Call
//   (ScriptNatives.cpp, Address Library 104651) counts a call whose Stack's
//   callback is one of them as the platform's and raises the depth for its
//   extent, as the direct path's Scope does. This replaced the (row, self)
//   notes with their 15 s expiry and the known limit of a script's call on
//   the same self inside the window.
namespace PlatformCall {
inline thread_local int t_depth = 0;

struct Scope
{
  Scope() { ++t_depth; }
  ~Scope() { --t_depth; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

inline bool Active()
{
  return t_depth > 0;
}

inline std::mutex& CallbackMutex()
{
  static std::mutex m;
  return m;
}

// the IStackCallbackFunctor pointers of the platform's VmCallbacks whose
// dispatch is pending or whose stack has not returned yet
inline std::unordered_set<const void*>& LiveCallbacks()
{
  static std::unordered_set<const void*> set;
  return set;
}

// the platform is about to dispatch a latent call with this callback: noted
// before the dispatch, since the VM may start the stack before the
// dispatcher returns
inline void NoteCallback(const void* callback)
{
  if (!callback) {
    return;
  }
  std::lock_guard<std::mutex> lock(CallbackMutex());
  LiveCallbacks().insert(callback);
}

// the callback ran (the call's stack returned), the dispatch was refused, or
// the callback died unrun: the mark is spent
inline void ForgetCallback(const void* callback)
{
  if (!callback) {
    return;
  }
  std::lock_guard<std::mutex> lock(CallbackMutex());
  LiveCallbacks().erase(callback);
}

inline bool IsLiveCallback(const void* callback)
{
  if (!callback) {
    return false;
  }
  std::lock_guard<std::mutex> lock(CallbackMutex());
  return LiveCallbacks().count(callback) > 0;
}

// the boundary's mark (thuum docs/DECISIONS.md ADR-068 decision 7,
// docs/research/scripts-one-system-2026-11-05.md 6.7): inside a script
// action, a second thread-local depth, raised by the Call hook
// (ScriptNatives.cpp) through the bracket below for the extent of an owned
// action row's call and read by the engine-level animation hooks through
// ScriptNatives::InScriptAction(), so the animation a script's
// PlayAnimation starts is reported once, by the boundary
inline thread_local int t_actionDepth = 0;

inline bool InAction()
{
  return t_actionDepth > 0;
}

// the Call hook's bracket on the running thread: Gum's onEnter and onLeave
// nest strictly on one thread (a call returns where it began), so two bits
// per nesting level say which marks that level raised (the platform's depth,
// the script action's), and the matching leave lowers them, never another
// level's. Call nests on the OnUpdate thread already (the platform's direct
// natives run inside the OnUpdate native's own Call), hence stacks of bits
// and not one flag; a level past the stacks' width is never raised, so no
// raise is ever lost. A throw or longjmp through a hooked Call skips its
// leave and would strand the level (a stale raised level keeps a depth at 1
// on that thread for the process's life, a stale unraised one drifts the
// nesting), so every level records its entry stack pointer and the next
// enter pops the levels whose recorded pointer sits at or below its own:
// x64's stack grows down, so a live enclosing frame's entry pointer is
// strictly above a nested call's (the N1 note of the flip's review,
// 2026-11-03).
inline thread_local std::uint64_t t_callRaised = 0;
inline thread_local std::uint64_t t_callAction = 0;
inline thread_local unsigned t_callNest = 0;
inline thread_local std::uintptr_t t_callSp[64] = {};

inline void LeaveCall();

// levels this thread left without a leave (a throw through Call): a
// recorded entry sp at or below this call's cannot be a live frame. The
// hook runs it first, before it reads a mark (Active(), InAction()), and
// EnterCall again, a no-op by then
inline void PopStrandedCalls(std::uintptr_t sp)
{
  while (t_callNest > 0 && t_callNest <= 64 &&
         t_callSp[t_callNest - 1] <= sp) {
    LeaveCall();
  }
}

inline void EnterCall(bool raise, bool action, std::uintptr_t sp)
{
  PopStrandedCalls(sp);
  const bool tracked = t_callNest < 64;
  raise = raise && tracked;
  action = action && tracked;
  if (tracked) {
    t_callSp[t_callNest] = sp;
    t_callRaised = (t_callRaised << 1) | (raise ? 1u : 0u);
    t_callAction = (t_callAction << 1) | (action ? 1u : 0u);
  }
  ++t_callNest;
  if (raise) {
    ++t_depth;
  }
  if (action) {
    ++t_actionDepth;
  }
}

inline void LeaveCall()
{
  if (t_callNest == 0) {
    // a leave with no enter of ours: the thread was inside Call when the
    // hook attached
    return;
  }
  --t_callNest;
  if (t_callNest < 64) {
    if ((t_callRaised & 1u) != 0) {
      --t_depth;
    }
    if ((t_callAction & 1u) != 0) {
      --t_actionDepth;
    }
    t_callRaised >>= 1;
    t_callAction >>= 1;
  }
}
}
