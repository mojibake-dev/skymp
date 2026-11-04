#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_set>

// thuum docs/DECISIONS.md ADR-062 decision 2 (slice 1b's reviews, 2026-11-03;
// the flip's native lane, docs/verbs/papyrus-events.md): the platform's own
// native calls reach the same entries the script hooks watch
// (ScriptNatives.cpp), so a hook must tell the platform's call from a script
// frame's. One mark, a thread-local depth that Active() reads at the entry,
// raised on two paths:
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
//   (ScriptNatives.cpp, Address Library 104651) raises the depth for the
//   call's extent when the Stack's callback is one of them, so the entry sees
//   Active() on the running thread exactly as the direct path does. This
//   replaced the (row, self) notes with their 15 s expiry and the known limit
//   of a script's call on the same self inside the window.
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

// the Call hook's bracket on the running thread: Gum's onEnter and onLeave
// nest strictly on one thread (a call returns where it began), so a bit per
// nesting level says whether that level raised the depth, and the matching
// leave lowers it, never another. Call nests on the OnUpdate thread already
// (the platform's direct natives run inside the OnUpdate native's own Call),
// hence a stack of bits and not one flag; a level past the stack's width is
// never raised, so no raise is ever lost. A throw or longjmp through a hooked
// Call skips its leave and would strand the level (a stale raised level keeps
// the depth at 1 on that thread for the process's life, a stale unraised one
// drifts the nesting), so every level records its entry stack pointer and the
// next enter pops the levels whose recorded pointer sits at or below its own:
// x64's stack grows down, so a live enclosing frame's entry pointer is
// strictly above a nested call's (the N1 note of the flip's review,
// 2026-11-03).
inline thread_local std::uint64_t t_callRaised = 0;
inline thread_local unsigned t_callNest = 0;
inline thread_local std::uintptr_t t_callSp[64] = {};

inline void LeaveCall();

inline void EnterCall(bool raise, std::uintptr_t sp)
{
  // levels this thread left without a leave (a throw through Call): a
  // recorded entry sp at or below this call's cannot be a live frame
  while (t_callNest > 0 && t_callNest <= 64 &&
         t_callSp[t_callNest - 1] <= sp) {
    LeaveCall();
  }
  const bool tracked = t_callNest < 64;
  raise = raise && tracked;
  if (tracked) {
    t_callSp[t_callNest] = sp;
    t_callRaised = (t_callRaised << 1) | (raise ? 1u : 0u);
  }
  ++t_callNest;
  if (raise) {
    ++t_depth;
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
    t_callRaised >>= 1;
  }
}
}
