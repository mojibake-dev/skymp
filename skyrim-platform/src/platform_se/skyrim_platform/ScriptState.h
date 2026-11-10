#pragma once

#include <RE/B/BSCoreTypes.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RE {
class TESObjectREFR;
}

// thuum docs/DECISIONS.md ADR-068 rung 3 (the script record) and its
// amendment A; the build plan docs/research/rung3-script-record-2026-11-06.md
// section 1(a): a world object's Papyrus script state read on its owner,
// written over a new owner's or a late joiner's copy, and the engine's
// lifecycle events raised to it again once written. Three TESModPlatform
// natives (PapyrusTESModPlatform.cpp; declared in TESModPlatform.psc and the
// committed .pex), a fourth over the third's mechanism for a relayed trigger
// entry (SendTriggerEvent, ADR-068 D-C2 A), and two sets the Papyrus event
// gate reads (FridaHooks.cpp OnSendEventEnter): the references whose served
// record waits for its apply (setScriptRecordPending, PuppetApi.cpp) and the
// lifecycle events the engine served per handle since the last load. The
// engine facts are docs/research/adr065-papyrus-state-2026-11-03.md Q1 to Q4
// (the VM's per-object layout, FindBoundObject, the engine's own save and
// load, the event gate) and H-R1 (CONFIRMED by trace,
// docs/verbs/papyrus-events.md: an object's variables are positional,
// parent-first, their count its type chain's total); every engine claim
// CommonLibSSE-NG does not state is a HYPOTHESIS in ScriptState.cpp, named by
// the plan's section 5 (H-R2 to H-R9; SendTriggerEvent's H-W3 by
// docs/research/fifty-fixes-2026-11-07.md section 4). Each native runs inside
// TESModPlatform.Add's Call on the OnUpdate thread (callNative's direct path,
// SkyrimPlatform.cpp RunInIOContext), and no C++ exception leaves one: past
// Add, a throw would freeze the seat (ChangeForm.cpp's note on the public
// natives).
namespace ScriptState {

// SerializeScriptState(refr): the reference's attached scripts as the record
// carries them, read under the VM's attachedScriptsLock, then its
// runningStacksLock (VirtualMachine.h:219, :210), the engine's own SaveGame
// order. A JSON object as a string:
//   { formId, quiescent: true, fnv1a (u32: FNV-1a 32 over the dump of
//     `scripts` with every `names` removed), seatLocal (the object values
//     written as None for having no portable handle), scripts: [{ type,
//     state, flags (Object::Save's byte: 0x01 constructed, 0x02 valid, 0x04
//     a lock holder, 0x08 initialized, 0x10 the lock pointer's flag bit),
//     names (the variables' names, for the lab), vars: [{ k (the value's
//     folded type: 0 None, 1 object, 2 string, 3 int, 4 float, 5 bool, 10 to
//     15 an array), then i, f, b or s for 2 to 5, o for an object ({ kind,
//     mid, form, type }; absent for a typed None), elem (an object array's
//     element class) and a (the elements, each { k, i|f|b|s|o }; absent for
//     a None array) }] }] },
// or, when a stack is on the object or it is locked or not initialized,
//   { formId, quiescent: false, busy: "stack 4711 state 3 in
//     defaultPuzzlePillarScript.RotatePillarToState" },
// or "" (JS null) when refused, the reason logged as `[verb:scripts]
// SerializeScriptState refused <id>: <reason>`: no reference, an actor, a
// runtime reference (0xff000000 and up), a trap family reference, no
// attached script, a class chain not linked, a non-finite float, a string
// past 1024 bytes or not UTF-8, a name past 128, an array past 128 elements,
// more than 16 scripts or 512 variables, an answer over 160 KiB.
std::string Serialize(RE::TESObjectREFR* refr);

// ApplyScriptState(refr, scriptsJson): a served record's scripts written over
// this seat's copies under the same two locks, held across the checks and
// every write. scriptsJson is the record's scripts as a JSON array, [{ type,
// state, vars }] in Serialize's shapes (names and flags ignored); an object
// carrying a `scripts` array is read the same way. Each script is matched to
// the attached object of exactly its type (the engine loader's exact match,
// adr065 Q2), its variable count checked against the chain's total, each
// value's kind against the declared type, its state against the chain's
// states, all before any of its writes; a script failing a check is refused
// whole and the others still apply. The answer, a JSON object as a string:
//   { formId, applied (true: the record's scripts were written under the
//     locks, any one its own checks refused named in scripts), retry (with
//     applied false: true for a moment's refusal, a stack on an object, a
//     lock held, an object not initialized, no script bound yet; false for
//     a record that cannot apply here), reason, scripts: [{ type, applied,
//     reason, vars }], resolved (object values found or kept), none (object
//     values written as None: the record's own, unresolved ones), unresolved,
//     seatLocal (handles of kind 1 or 2 written as None, H-R6),
//     stacksOnObject (stacks on the objects walked after the writes, still
//     under the locks: H-R4 reads 0), holdUs (the locks' hold), blanked (the
//     names the gate blanked for the reference since its pending mark was
//     set), fnvAfter (Serialize's fnv1a of this seat's scripts after the
//     writes, the new owner's baseline) }; a refusal carries formId,
//     applied false, retry, reason and blanked.
// An applied record lifts the reference's pending mark and takes its
// blanked names with it; a refusal keeps both (skymp5-client retries, and
// its give-up lifts the mark).
std::string Apply(RE::TESObjectREFR* refr, std::string_view scriptsJson);

// SendScriptEvent(refr, name, unseenOnly): a lifecycle event (OnCellAttach,
// OnLoad or OnCellLoad, the three the seen set knows, each with no
// argument) sent to the reference's scripts and relayed to its aliases, as
// the engine's own sinks raise it (adr065 Q4: SendEvent, then
// SkyrimVM::RelayEvent, with a ZeroFunctionArguments), under the platform's
// mark; with unseenOnly, not when the engine served that event to the handle
// since the last load. A JSON object as a string: { formId, event, sent,
// scripts (the attached scripts it was sent to), seen (true when unseenOnly
// skipped it), reason }.
std::string Send(RE::TESObjectREFR* refr, std::string_view name,
                 bool unseenOnly);

// SendTriggerEvent(trigger, name, actionRef): thuum docs/DECISIONS.md ADR-068
// D-C2 A (docs/research/fifty-fixes-2026-11-07.md 1(b) and read 2): a guest's
// entry into a world trigger, relayed to the cell's host, played there.
// OnTriggerEnter or OnTriggerLeave (without case) sent to the trigger's
// scripts and relayed to its aliases with actionRef as the event's one
// argument, akActionRef: Send's mechanism with CommonLibSSE-NG's
// one-object FunctionArguments in place of the ZeroFunctionArguments, under
// the platform's mark (callNative's Override passes it at the gate, before
// the gate's presser rule reads any argument). A JSON object as a string:
// { formId, event, sent, scripts (the attached scripts it was sent to),
// reason }; refused for a name that is no trigger event, a None actionRef,
// an actor trigger and a reference with no attached script. Since rung 5 a
// figure presser (an actor at 0xff000000 and up) arms the trigger's acting
// scope (ActingScope.h) before the send, its server id from the client's
// entry request (setActingPlayer with stage -3), so the handler reads
// Game.GetPlayer() as the figure.
std::string SendTrigger(RE::TESObjectREFR* refr, std::string_view name,
                        RE::TESObjectREFR* actionRef);

// the references whose served script record waits for its apply, by form id
// (setScriptRecordPending: skymp5-client's ScriptStateService raises it for
// a record that carries its view and lowers it when it gives the record up,
// through releaseScriptRecordPending, which hands back the names blanked; an
// applied record lowers it too). Hosted.h's idiom: mutex-guarded,
// the size kept for a read with no lock. The gate blanks OnLoad and
// OnCellLoad to a pending reference and keeps the names it blanked for the
// apply's answer
namespace Pending {
bool Is(std::uint32_t formId);
// a raise keeps the names a pending mark already holds; a fresh mark starts
// with none; a lowering drops the mark and its names (Release hands them
// back)
void Set(std::uint32_t formId, bool pending);
// releaseScriptRecordPending's table operation: the mark lowered and the
// names the gate blanked while it was up answered as a JSON array of
// strings, "[]" when none or when no mark was up; no game or VM call
std::string Release(std::uint32_t formId);
std::size_t Count();
// any reference in the set, one atomic read: the gate asks it first
bool Any();
// OnLoad or OnCellLoad, without case: the two names the gate blanks
bool IsBlankedName(const char* eventName);
// the gate blanked `eventName` to the pending reference
void NoteBlanked(std::uint32_t formId, const char* eventName);
}

// the lifecycle events the engine served per VM handle (OnCellAttach, OnLoad,
// OnCellLoad): noted by the gate when it passes one (and by Send), the
// handle's entry dropped at its OnCellDetach, the whole set cleared at
// TESLoadGameEvent (PapyrusTESModPlatform.cpp's load sink)
namespace Seen {
// the gate's observation of one event: served, a lifecycle event is noted;
// OnCellDetach drops the handle's entry whether served or not
void OnEvent(RE::VMHandle handle, const char* eventName, bool served);
bool Has(RE::VMHandle handle, std::string_view eventName);
void Clear();
}
}
