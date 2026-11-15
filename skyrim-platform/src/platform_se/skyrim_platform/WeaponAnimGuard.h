#pragma once

#include <cstdint>
#include <functional>

namespace RE {
class Actor;
}

// thuum lab/.cache/drafts/reads-53/r-c3-temple-crash.md (playtest 51 break
// 7, R-C3) and x-anim-listener 20261115-051655 (train 53 da5707f7, no
// guard; its read and adversarial check): c1 died in the animation file
// manager's dispatcher (63997+0xC1, a BSJobs thread) calling a
// WeaponAnimationLoadedCallback (VTABLE 207865, CommonLibSSE-NG include/RE/
// Offsets_VTABLE.h:2210) already freed. On the dead copy 0x375aa npc-fate's
// reconcile dressed it (FateApi.cpp's dead branch: EquipObject, Actor::
// OnItemEquipped, slot 0xB2, include/RE/A/Actor.h:376, then 39295), which
// made request 1 and its listener L, held at MiddleHighProcessData::unk1F0
// (include/RE/M/MiddleHighProcessData.h:166) with one reference; 43 ms later
// the record's apply (ChangeForm.cpp) ran its finish pass (35567, Actor::
// FinishLoadGame 37652, Actor.h:278 slot 0x11, then 39432), which made
// request 2 on L raw; request 1's completion freed L on thread 1028 (39296),
// and request 2's dispatch called it: two dress passes on one actor in one
// load, the second while the first's request was outstanding.
//
// The one gate every pass that dresses an actor on the main thread goes
// through first (FateApi.cpp: the re-roll's decision, the redress, the
// body's dress and the stand-up; ChangeForm.cpp: the record's apply), so an
// actor gets one dressing pass at a time per load:
//   - a dead copy whose record's apply is queued or running is the apply's
//     (ChangeForm::HoldForApply): its LoadGame and FinishLoadGame lay the
//     record's worn list (ADR-065, the save is the state), and the client's
//     reconcile after the lay (changeFormService.ts onApplyResult) is the
//     fate's one pass; the work held is run after a refused apply;
//   - nothing runs while a weapon-animation request is outstanding on the
//     actor's process: its handle at the record's +0x40 or its listener at
//     unk1F0 set (Peek); the work goes to a later tick, bounded.
// Every wait, skip, fold and drop is said with its actor's id, one line per
// work (never per tick), and counted (ADR-073 decision 7).
// HYPOTHESIS, every engine part, until a counted x-anim-listener run on a
// build with this reads no pendingAtFree weapon dtor, no flag row and no
// request row whose listener an earlier request holds (the probe's (i) to
// (iii)).
namespace WeaponAnimGuard {

// the work a deferral holds up; each has its own counts
enum class Kind : std::uint8_t
{
  kReroll,    // FateApi.cpp rerollActor: a dead copy's reconcile, a redress
              // alone, or 37323's re-roll and its rebuild
  kRedress,   // FateApi.cpp redressActor: the outfit's redress, or a dead
              // copy's reconcile
  kBody,      // FateApi.cpp dressBody: a dead copy's record armor worn again
  kResurrect, // FateApi.cpp resurrectActor: Actor::Resurrect, whose inventory
              // reset re-arms and whose attach re-equips
  kLoadGame,  // ChangeForm.cpp: the record's apply (35568's Actor::LoadGame,
              // slot 0x0F, then 35567's FinishLoadGame)
  kCount
};

enum class Decision : std::uint8_t
{
  kRun,      // nothing outstanding: the caller does its work now
  kDeferred, // a request or its listener outstanding: `again` runs next tick
  kFolded,   // this work for this actor already waits: the caller drops it
  kDropped,  // still outstanding after kMaxTicks: the work is not done
  kHeld,     // a dead copy's record apply is queued or running: the apply
             // took the work (`owed`, run if it answers refused)
};

// the ticks a deferral waits, each one gameThrQ drain (main.cpp's OnUpdate)
// and then the main thread's UI drain, before the work is dropped
inline constexpr std::uint32_t kMaxTicks = 120;

// What the engine holds outstanding for the actor's weapon animation (REA
// and the ghidra MCP on sky-re, 1.6.1170, HYPOTHESIS): 39295 (0x1406e0070),
// under the spinlock at unk1E8 (MiddleHighProcessData.h:165), builds the
// listener, sets unk1F0 to it with a reference and stores 39778's request
// at the record unk1B8's first word points to, +0x40; the listener's
// handler 40110 zeroes +0x40 (0x1407217dd) on the dispatcher's thread and
// 39296 (0x1406e0320, reached through slot 0xB2) then releases unk1F0, so
// between the two the listener alone is set (051655 row 139, window (a)).
// 39432 (0x1406e97b0) reads unk1F0 without the lock and hands it raw to its
// own request when set (0x1406e9b6d to 0x1406e9bc0); when null its request
// carries 40071, whose body (0x140717750) swaps a loaded set into the record
// and calls no listener. Read without the lock, as 39432 reads: a stale
// read costs one tick more.
struct InFlight
{
  const void* request = nullptr;  // the record's +0x40
  const void* listener = nullptr; // unk1F0
  bool Any() const { return request != nullptr || listener != nullptr; }
};
InFlight Peek(RE::Actor* actor);

// On the main thread, inside the task, before any engine work on the actor.
// tick is 0 for the first ask and the deferral's count after; again(tick + 1)
// queues the work anew and runs on the thread that drains gameThrQ. owed,
// when given, makes the work one a dead copy's record apply takes (kHeld):
// it runs on the main thread after the apply answers refused, and is
// dropped after a lay. A deferred work that ends without asking again (its
// actor gone, its queue refused) calls Forget so a later ask is not folded
// into nothing.
Decision Check(Kind kind, RE::Actor* actor, std::uint32_t refrId,
               std::uint32_t tick, std::function<void(std::uint32_t)> again,
               std::function<void()> owed = nullptr);
void Forget(Kind kind, std::uint32_t refrId);
}
