#pragma once

#include <cstdint>
#include <functional>

namespace RE {
class Actor;
}

// thuum lab/.cache/drafts/reads-53/r-c3-temple-crash.md (playtest 51 break
// 7, R-C3; the re-analyst's REA read of 2026-11-14): rotfern's game crashed
// entering Bleak Falls Temple inside the animation file manager's dispatcher
// of finished requests (63997+0xC4, on a BSJobs thread), which called a
// listener (IAnimationSetCallbackFunctor, CommonLibSSE-NG include/RE/I/
// IAnimationSetCallbackFunctor.h:13) already freed, 0.3 s after this seat's
// fate burst (arm, re-roll) on the Temple's live NPCs and its change-form
// applies (Actor::LoadGame) on the dead. Candidate A (medium): a
// WeaponAnimationLoadedCallback (VTABLE 207865, include/RE/Offsets_VTABLE.h:
// 2210) released by OnItemEquipped's finish (39296) or a new equip (39295)
// while its request still waits, since the request holds the listener as a
// raw pointer and only a cancel (63971) unhooks it. The guard every fate task
// and the change-form apply pass first: none of them touches an actor while
// the engine holds a weapon-animation request for it; the work goes to a
// later tick, bounded, and its first deferral of each kind is said once.
// HYPOTHESIS, every part, until lab/frida/anim-listener.js reads candidate A
// (x-anim-listener by path, lab/.cache/drafts/train53/probes).
namespace WeaponAnimGuard {

// the work a deferral holds up; each has its own once-per-kind lines and
// counts (ADR-073 decision 7's merge rule)
enum class Kind : std::uint8_t
{
  kReroll,    // FateApi.cpp: 37323 and its rebuild (an arm, Load3D, 3D passes)
  kResurrect, // FateApi.cpp: Actor::Resurrect, whose inventory reset re-arms
  kLoadGame,  // ChangeForm.cpp: the record's apply, Actor::LoadGame (0x0F)
  kCount
};

enum class Decision : std::uint8_t
{
  kRun,      // no request in flight: the caller does its work now
  kDeferred, // a request in flight: `again` runs on the next tick
  kFolded,   // this work for this actor already waits: the caller drops it
  kDropped,  // still in flight after kMaxTicks: the work is not done
};

// the ticks a deferral waits, each one gameThrQ drain (main.cpp's OnUpdate)
// and then the main thread's UI drain, before the work is dropped
inline constexpr std::uint32_t kMaxTicks = 120;

// The request the engine holds for the actor's weapon animation, or nullptr:
// MiddleHighProcessData::unk1B8 (include/RE/M/MiddleHighProcessData.h:163)
// is typed BSTArray<void*>, but the engine reads its first word as a pointer
// to a record whose +0x40 holds the request 39778 returned (39295 at
// 0x1406e026c, 39296 at 0x1406e0411 and 0x1406e0431, REA); callback, when
// given, the WeaponAnimationLoadedCallback held at +0x1F0 (:166). A peek
// without the spinlock at +0x1E8 (:165) that 39295 and 39296 take: a stale
// read costs one tick more, or one run as before this guard. Nothing on SE.
const void* InFlightRequest(RE::Actor* actor, const void** callback = nullptr);

// On the main thread, inside the task, before any engine work on the actor.
// tick is 0 for the first ask and the deferral's count after; again(tick + 1)
// queues the work anew and runs on the thread that drains gameThrQ. A
// deferred work that ends without asking again (its actor gone, its queue
// refused) calls Forget so a later ask is not folded into nothing.
Decision Check(Kind kind, RE::Actor* actor, std::uint32_t refrId,
               std::uint32_t tick, std::function<void(std::uint32_t)> again);
void Forget(Kind kind, std::uint32_t refrId);
}
