import { MsgType } from "../../messages";
import { ScriptObjectState } from "./scriptStateTypes";

// thuum docs/DECISIONS.md ADR-068 decision 5 and amendment A (rung 3, the
// script record): a recorded ScriptState served to a seat. Server to client,
// reliable: to a cell's new host ahead of its HostGrant (each reference of the
// cell with a record), and to a subscriber that does not host the cell right
// behind the reference's create word, a late joiner and a restart included.
// The record carries its view (amendment A: defaultPuzzlePillarScript's
// OnCellLoad turns the graph once, from its first side, so a re-sent load
// event cannot render a pillar from its state): the reference's last
// animation event when the record was taken and the object's seq then (the
// seam's per-object sequence, ADR-068 decision 4). ScriptStateService holds it
// (the platform's pending mark when it carries a view, so the gate holds the
// reference's OnLoad and OnCellLoad), applies it through ApplyScriptState on
// the update, re-sends the events the gate held, then replays the view where
// this seat's seq for the form is another. Safe to receive twice: the newer
// record replaces a held one. JSON field names as wire-schema's
// ScriptStateApply reads them

// thuum docs/research/fifty-fixes-2026-11-07.md 1(a), D-C3 A (PENDING ELI):
// one graph variable of the reference as the server keeps it from its owner's
// SetAnimationVariableBool, Int or Float, with the object's seq of that call
// (each variable its own, the newest per name wins): k by ScriptVar's folded
// kinds, 3 int (i), 4 float (f, finite), 5 bool (b), and that payload alone
export interface GraphVar {
    name: string;
    k: number;
    i?: number | null;
    f?: number | null;
    b?: boolean | null;
    seq: number;
}

export interface ScriptStateApplyMessage {
    t: MsgType.ScriptStateApply;
    formId: number;          // the reference's form id, a game-file reference's
    fnv1a: number;           // the record's FNV-1a 32, as its owner read it
    seq: number;             // the object's seq when the record was taken
    event?: string | null;   // the record's view, the reference's last animation event then; absent or null for none
    scripts: ScriptObjectState[];
    // D-C3 A (PENDING ELI): the reference's graph variables as kept at the
    // serve, at most 16; absent or null for none
    graphVars?: GraphVar[] | null;
}
