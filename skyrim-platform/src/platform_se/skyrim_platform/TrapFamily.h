#pragma once

#include <RE/B/BSCoreTypes.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RE {
class TESObjectREFR;
namespace BSScript {
class ObjectTypeInfo;
}
}

// thuum docs/DECISIONS.md ADR-068 D-S3 (traps follow the interaction,
// accepted 2026-11-05; docs/research/trap-slice-d-s3-2026-11-05.md): the
// trap family, one test shared by the Papyrus event gate (FridaHooks.cpp
// OnSendEventEnter), the native seam (ScriptNatives.cpp OnCall) and
// skymp5-client (getTrapRole, PuppetApi.cpp). A reference is in the family
// when every script attached to it is a trap class (its parent chain reaches
// one of the family's roots, or its own name begins with the family's
// prefix, both named in one place in TrapFamily.cpp; Eli's D-T1) and its
// base is no inventory object (a pickup is the inventory's). Every seat runs a
// family reference's scripts itself: the gate passes its events in any cell
// and the seam counts it as every seat's own, so a trap fires wherever its
// trigger is met, and what crosses is its single-use result (DestructionStage,
// from the batch's SetDestroyed) and its bodies' motion (the prop stream, from
// the batch's SetMotionType and ApplyHavokImpulse). A trap's trigger is
// never such a body (playtest 48, 2026-11-06): getTrapRole's trigger names a
// reference whose script reaches TrapTriggerBase (the plates, the tripwire,
// the bone alarms, the linkers), and skymp5-client claims and follows none,
// so each seat's own copy moves its own (Embershard's wire 0xb6d3f drops its
// rope by its own Tripwire state's SetMotionType). Beside the test, two sets
// in Hosted.h's idiom, written by skymp5-client on the JS thread with no game
// call: the trap bodies another seat streams, and the references whose
// server record is being applied.
namespace TrapFamily {
// a script class is in the family: its own name begins with the prefix, or
// it, or a class it extends, is one of the roots; the parent chain is walked
// once per class and the verdict kept, with the class's trigger verdict
// beside it (RoleOf's), so the gate's cost stays flat (rule 9)
bool IsFamilyType(const RE::BSScript::ObjectTypeInfo* info);
// the reference is in the family: its base is no inventory object and every
// script attached to its VM handle is in the family; false for a null
// reference or a handle with no attached script. The VM's attached scripts
// are read under the VM's own lock, the gate's walk
bool IsFamily(const RE::TESObjectREFR* ref, RE::VMHandle handle);
// a trap's trigger: a script attached to the handle reaches TrapTriggerBase
// in its parent chain (the per-class verdict IsFamilyType keeps, getTrapRole's
// trigger); false for a handle with no attached script. Read under the VM's
// own lock, the gate's walk. D-W2 A's firing window opens for a trigger alone
// (D-W2's fresh review, note 1)
bool IsTrigger(RE::VMHandle handle);
// the reference's destroyed flag (TESForm's kDestroyed, the bit
// ObjectReference.SetDestroyed sets and clears)
bool Destroyed(const RE::TESObjectREFR* ref);

// getTrapRole's answer for a reference with an attached script; no answer
// (std::nullopt) for an id that names no reference or one with no attached
// script. Read on the calling thread: the form table under its own lock, the
// attached scripts under the VM's. trigger: any attached script reaches
// TrapTriggerBase in its parent chain (the per-class verdict IsFamilyType
// keeps), read whether or not the reference is in the family. exempt
// (ADR-068 D-W2 A's fallback lever, Exempt below): a script attached to it
// is of a class the exempt list names, chain-matched without case, read
// whether or not the reference is in the family (the presser rule applies
// the list to family references alone), so skymp5-client skips its root
// report for a trigger every seat fires itself
struct Role
{
  bool family = false;
  bool playerOnly = false;
  bool destroyed = false;
  bool trigger = false;
  bool exempt = false;
};
std::optional<Role> RoleOf(uint32_t formId);

// D-S3 point 4: the trap bodies another seat streams, by form id
// (setPropFollowed: skymp5-client's prop stream at the owner word and at
// the owner's at-rest sample). The seam runs this seat's script
// SetMotionType and ApplyHavokImpulse on such a body with a neutral argument
// (ScriptNatives.cpp)
namespace Followed {
bool Is(uint32_t formId);
void Set(uint32_t formId, bool followed);
size_t Count();
}

// D-S3 point 3: the references whose server record skymp5-client is
// applying, by form id (setRecordApplying around the apply). The gate blanks
// every Papyrus event to such a reference meanwhile (FridaHooks.cpp)
namespace RecordApplying {
bool Is(uint32_t formId);
void Set(uint32_t formId, bool applying);
size_t Count();
// any reference in the set, one atomic read: the gate asks it on every
// event before it resolves the event's self
bool Any();
}

// thuum ADR-068 D-W2 A's fallback lever (docs/research/
// fifty-fixes-2026-11-07.md section 7.2(a), the decision's "keeps D-S3's
// per-seat firing for that case"; setTriggerPresserExempt, PuppetApi.cpp):
// the script classes whose trap family references keep D-S3 under the
// presser rule (FridaHooks.cpp OnSendEventEnter: a non-owned press to such a
// reference passes as before the rule, and an owned one opens no look
// window, so no look crosses and every seat fires its own copy), and which
// getTrapRole names exempt. Chain-matched without case, the gate's
// admitted-script walk. Empty by default and replaced whole at each Set; a
// list, the design law's debt, used only on a red. Written on the JS thread,
// read on any: the names are one immutable snapshot swapped under a mutex,
// so no read holds that mutex beside the VM's lock
namespace Exempt {
void Set(const std::vector<std::string>& classes);
// any class named, one atomic read
bool Any();
// a script attached to the handle is of an exempt class (the VM's attached
// scripts under its own lock)
bool Is(RE::VMHandle handle);
// the names as stored (lowercased), comma-separated, for the setter's line
std::string Names();
}

// thuum docs/DECISIONS.md ADR-068 D-W2 A (Eli 2026-11-07; docs/research/
// fifty-fixes-2026-11-07.md section 7.2(a)): the trap family triggers whose
// owned trigger event the presser rule passed on this seat (FridaHooks.cpp
// OnSendEventEnter, its owned branch, on an enter and on a leave alike), by
// form id with the time of the last one: the window in which a trigger's own
// calls on itself are its firing seat's look (ScriptNatives.cpp
// OnFamilyCall), so the plate's 'Up' after its owner's leave is inside. In
// Hosted.h's idiom: mutex-guarded, the size kept for a read with no lock,
// cleared at a game load (PapyrusTESModPlatform.cpp's load sink)
namespace Firing {
// LOOK_MS, UNMEASURED (HYPOTHESIS H-LK2: a trigger's look calls fall inside
// it, x-trap-look-both v1's look lines against the presser rule's)
constexpr uint64_t kLookMs = 10000;
void Note(uint32_t formId, uint64_t nowMs);
// within kLookMs of the reference's last note
bool Open(uint32_t formId, uint64_t nowMs);
// any reference noted since the last clear, one atomic read: the seam asks
// it before it takes the lock
bool Any();
void Clear();
size_t Count();
}

// the install line (once per process, at kDataLoaded beside the seam's
// table line): the family's roots and the three exports, so a run's log
// names the build's family
void LogReady();
}
