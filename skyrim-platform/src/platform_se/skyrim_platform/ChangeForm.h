#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace RE {
class TESObjectREFR;
}

// thuum docs/verbs/change-form.md (ADR-065, system 1: the save is the state;
// the first trigger the corpse, ADR-067): the engine's own change form of a
// loaded actor, produced on demand on the owner and applied on a watcher
// through the engine's own LoadGame and finish pass. The two natives below
// are the C++ of lab/frida/serialize-on-demand-actor.js v2.2 (the recipe and
// the id census, runs 20261103-153929 and 192013) and lab/frida/
// apply-on-demand.js v2.3 (the REMAP step and the apply, runs 164416, 174905,
// 193005 and 210713), in CommonLibSSE-NG types; the engine facts are
// docs/research/adr065-serialize-on-demand-2026-11-03.md (the id tables, Q1
// to Q4, the (c) read, D6) and docs/engine/change-forms.md C4 to C6. Every
// Address Library id is game/Offsets.h's ChangeForm block or CommonLibSSE-NG's
// own header; nothing here is from memory (thuum CLAUDE.md rule 1), and every
// engine body named in a comment is a HYPOTHESIS by rule 2 until the lab's
// run says otherwise.
namespace ChangeForm {

// The two seat-side hooks of this slice, installed once from
// Frida::InstallHooks (AE only, 1.6.1170's ids): the id census, a Gum
// replacement of BGSNumericIDIndex::SetNumericID (35928) that records every
// 3-byte id the recipe writes while a serialize runs on this thread and is
// inert otherwise (filtered in native, rule 9); and the address proof line,
// one `[verb:change-form] address` row per id with its RVA and first bytes
// against the unpacked image's (the T1 of this slice: the fork has no
// host-less harness, so the ready log is the proof the relocations resolve).
void InstallHooks();

// SerializeChangeForm(refr): the on-demand recipe on the reference's form
// (an ACHR in this slice), on the calling thread: a scratch BGSSaveFormBuffer
// (36035, the header 36037 from the entry's flags, the live form type and
// 35740's version), CheckSaveGame (slot 0x0D), the initial data (35715),
// SaveGame (slot 0x0E), the pack (36081); the record's bytes copied out, the
// record freed with the engine's allocator and the buffer destroyed (36044).
// The census hook runs for the recipe's duration and gives the remap rows
// (D6): one {offset, formId, index} per type-0 id in the body, offset the
// RECORD offset of the three bytes (the buffer position at the write plus
// the record header's width), formId the sender's full form id.
// Returns a JSON object as a string:
//   { formId, formType (u8, the live form's type byte), recordType (u8, the
//     record's save-type index, 1 for ACHR), sizeCode, flags (u32, the
//     header flags CheckSaveGame kept: what the apply writes), entryFlags,
//     dropped, version (u8), kind (the initial data's), bodyHex (the whole
//     record's bytes), bytes, fnv1a (u32 over those bytes), plainBytes,
//     plainFnv1a, deflated (false: a deflated record is refused), remap:
//     [{offset, formId, index}], remapTruncated, census: {rows, type0,
//     type1, type2, type3, nulls, headerKind, dropped}, entryUntouched,
//     thread, mainThread, onMainThread, ms }
// or "" (JS null) when a premise refused, the reason in skyrim-platform.log
// as `[verb:change-form] SerializeChangeForm refused`, or when the body
// threw (logged as `SerializeChangeForm ... threw`; a throw never leaves the
// native: past TESModPlatform::Add it would freeze the seat).
std::string Serialize(RE::TESObjectREFR* refr);

// ApplyChangeForm(refr, bodyHex, flags, version, remapRows): a served record
// applied to this seat's LOADED copy: the record parsed and checked against
// the header and the engine's version, the REMAP check (each row's three
// bytes decode to kind 0 with the row's index, the form found here by
// TESForm::LookupByID with the row's type), the REMAP encode through the
// engine's own 35928 on a scratch and the bytes patched, the heap copy
// through the engine's allocator (68115), the record written into the form's
// changes-map entry (35509), the apply (35568: the decode, the initial data,
// LoadGame, the pending array), the finish pass (35567: InitLoadGame, the
// flush of the queued Havok block to the standing 3D or its park for a
// leveled actor's rebuilt one, FinishLoadGame). The pose comes through the
// engine's own path (C4), nothing extra: no PARK, no suppress bit.
// remapRows is "offset:formId:index[:formType],..." (the Frida script's
// REMAP param; callNative takes no array). flags and version are the
// sender's `flags` and `version` as the serialize returned them (flags as a
// u32 reinterpreted as a Papyrus Int: `flags | 0` in JS).
// Returns a JSON object as a string:
//   { applied (bool), reason (string, "" when applied), fnvPatched (u32 over
//     the bytes 35509 handed the engine, 0 when refused), and the
//     diagnostics: formId, applyReturn, remapRows, rows: [{offset, formId,
//     fromIndex, toIndex}], havok: {present, count, blockStart, blockLen},
//     entryAfterApply, pendingAfterApply, pendingAfterFinish, rootBefore,
//     rootAfterApply, rootAfterFinish, lifeStateBefore, lifeStateAfter,
//     thread, mainThread, onMainThread, ms }.
// A throw inside the body answers { applied false, reason "exception: ..." }.
std::string Apply(RE::TESObjectREFR* refr, std::string_view bodyHex,
                  std::uint32_t flags, std::uint32_t version,
                  std::string_view remapRows);
}
