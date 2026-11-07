import { MsgType } from "../../messages";
import { ScriptObjectState } from "./scriptStateTypes";

// thuum docs/DECISIONS.md ADR-068 decision 5 and amendment A (rung 3, the
// script record; docs/research/rung3-script-record-2026-11-06.md 1(b) and
// 1(d)): a world object's Papyrus state as its owner's engine holds it, read
// by the platform's SerializeScriptState when the reference is quiescent
// (ScriptStateService: a call of the seam's batch on it or by it, one hop
// through another record's object handles, the cell's release, a 30 s
// sweep; one read a second at most, nothing sent when the FNV is the last
// sent). Client to server, reliable, R2: the owner computes, the server
// records it on the reference (never relayed live) and serves it as
// ScriptStateApply. The validator: formId not 0 and below 0xff000000, 1 to 16
// scripts with a type each, flags with 0x01 and 0x08 and without 0x04, each
// k in 0 to 5 or 10 to 15 with its own payload, a handle's kind 0 to 2 and its
// form neither 0x14 nor at 0xff000000 and up (E_VAL_RANGE), no non-finite
// float (E_VAL_NONFINITE), a burst of 64 then 8 a second; the server's rule:
// the sender the cell's host at the epoch, or the host just released at its
// own epoch while the cell is unhosted (E_SCRIPTSTATE_NO_REF, _KIND for an
// actor, _OWNER, _EPOCH). Safe to receive twice: a record replaces the last.
// JSON field names as wire-schema's ScriptState reads them
export interface ScriptStateMessage {
    t: MsgType.ScriptState;
    epoch: number;    // the cell's host epoch this client holds for the reference's cell (ADR-066's stamp, HostService.epochForActor), or the epoch just released for a release read
    formId: number;   // the reference's form id, a game-file reference's (never 0, below 0xff000000)
    fnv1a: number;    // FNV-1a 32 over the platform's dump of the scripts (its lab-only names left out), the owner's compare baseline
    scripts: ScriptObjectState[];
}
