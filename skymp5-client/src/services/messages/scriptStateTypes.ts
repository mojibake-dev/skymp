// thuum docs/DECISIONS.md ADR-068 decision 5 (rung 3, the script record;
// docs/research/rung3-script-record-2026-11-06.md 1(a) and 1(b)): one
// reference's Papyrus state as the engine's own save writes it (H-R1,
// CONFIRMED by x-script-save v1.1: per attached script its type, its state,
// its flags and every variable in positional order, parent first, the count
// the type chain's total), shared by ScriptState 76 and ScriptStateApply 77.
// JSON field names as wire-schema's ScriptHandle, ScriptValue, ScriptVar and
// ScriptObjectState read them; an absent optional is None on the wire

// an object variable's handle (the VM's own, adr065 Q1): kind 0 a plain
// form's (mid 0xFFFF, the form id low), portable between seats; kinds 1 and
// 2 name seat-local objects and are applied as None (H-R6)
export interface ScriptHandle {
    kind: number;  // the handle's kind, 0 to 2
    mid: number;   // its middle 16 bits
    form: number;  // the form id it names (never 0x14 and never 0xff000000 and up: the platform writes such a handle as None, "seatLocal")
    type: string;  // the object's script type ("defaultPillarPuzzleLever")
}

// one value by its folded type (CommonLibSSE-NG include/RE/T/TypeInfo.h:14-28,
// :52): k 0 None, 1 an object (`o`; none for a typed None), 2 a string (`s`),
// 3 an int (`i`), 4 a float (`f`, finite), 5 a bool (`b`)
export interface ScriptValue {
    k: number;
    i?: number;
    f?: number;
    b?: boolean;
    s?: string;
    o?: ScriptHandle;
}

// one variable: a value, or k 10 to 15 an array of kind k-10 (`a`, at most
// 128 elements, A/Array.h:69) with `elem` the element's type for an object
// array (11)
export interface ScriptVar extends ScriptValue {
    elem?: string;
    a?: ScriptValue[];
}

// one attached script (at most 16 a reference): its type, its current state
// ("" the empty state), Object::Save's flag byte (0x01 constructed, 0x02
// valid, 0x04 lock held, 0x08 initialized; the validator wants 0x01 and 0x08
// and refuses 0x04) and its variables in positional order (at most 512)
export interface ScriptObjectState {
    type: string;
    state: string;
    flags: number;
    vars: ScriptVar[];
}
