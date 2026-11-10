import { MsgType } from "../../messages";

// thuum docs/DECISIONS.md ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md
// 1(a); D-V1 B, D-T2 B): the variables INFO and SCEN conditions read
// (GetVMQuestVariable 629, GetVMScriptVariable 630), a quest's or a
// reference's attached script variables, R2: the writing seat's engine
// computes, the server records. Both directions, reliable. From the server:
// at a login the watch list (each key's own set, in messages of at most 256)
// and the served values (served true: the world's record, and this player's
// own record to it alone), then each relay of another seat's write to a world
// key (served false), every entry with the record's sequence for its key,
// which a receiver keeps, applying a value above the one it holds. The player
// cut (D-T2 B): a player-class quest's key is its player's own, `own` true on
// the watch and on every entry; its value is recorded on the player's own
// change form and never relayed (the marked quests' scalar variables, and
// the game-time registration's two reserved names). To the server: the
// watched keys whose values changed since the last read here, `watch` empty,
// `served` false, every `seq` 0, an own key's entry own true (a player's key
// reported as the world's, or a world key as own, is E_SCRIPTVAR_PLAYER).
// `own` and `seq` render on every entry and `own` on every key. The
// validator: at most 256 keys and 256 entries, a form not 0 and below
// 0xff000000, names neither empty nor carrying a NUL, a key named once in a
// list, a kind 3 to 5 with exactly its payload, a finite float, a served
// entry's seq never 0. JSON field names as wire-schema's ScriptVariables
// (ScriptVarKey, ScriptVarEntry) reads them
export interface ScriptVariableKey {
    form: number;   // the QUST or REFR, a load-order form id
    script: string; // the attached script whose chain declares the variable
    var: string;    // the variable's name as the compiled script holds it ("::vampirestatus_var")
    own: boolean;   // the key is its player's own (a player-class quest's)
}

export interface ScriptVariableEntry extends ScriptVariableKey {
    k: number;     // rung 3's folded kinds: 3 int, 4 float, 5 bool
    i?: number;    // the value of a kind 3
    f?: number;    // of a kind 4
    b?: boolean;   // of a kind 5
    seq: number;   // the server's sequence for the key, rising at each recorded write; 0 on a report
}

export interface ScriptVariablesMessage {
    t: MsgType.ScriptVariables;
    watch: ScriptVariableKey[];   // from the server at a login alone: the keys this seat reads once a second
    entries: ScriptVariableEntry[];
    served: boolean;              // true for the record served at a login
}
