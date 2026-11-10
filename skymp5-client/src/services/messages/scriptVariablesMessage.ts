import { MsgType } from "../../messages";

// thuum docs/DECISIONS.md ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md
// 1(a); D-V1 B, D-T2 B): the variables INFO and SCEN conditions read
// (GetVMQuestVariable 629, GetVMScriptVariable 630), a quest's or a
// reference's attached script variables, R2: the writing seat's engine
// computes, the server records. Both directions, reliable. From the server:
// the login's serve (served true) with the keys to watch and the world's
// values, then each relay of another seat's write (served false); its `own`
// entries, served to their player alone, are that player's own quests' state
// (D-T2 B: the marked quests' scalar variables and the game-time
// registration). To the server: the watched keys whose values changed since
// the last read here, never a watch, never served, never epoch 0, never
// `own` (the server's player cut names a key its player's). The latest
// write wins per key with the server's sequence. The validator: at most 256
// keys and 256 entries, a form not 0 and below 0xff000000, names neither
// empty nor carrying a NUL, a key named once, a kind 3 to 5 with its payload
// alone, a finite float. JSON field names as wire-schema's ScriptVariables
// reads them
export interface ScriptVariableKey {
    form: number;   // the QUST or REFR, a load-order form id
    script: string; // the attached script whose chain declares the variable
    var: string;    // the variable's name as the compiled script holds it ("::vampirestatus_var")
}

export interface ScriptVariableEntry extends ScriptVariableKey {
    k: number;   // rung 3's folded kinds: 3 int, 4 float, 5 bool
    i?: number;  // the value of a kind 3
    f?: number;  // of a kind 4
    b?: boolean; // of a kind 5
}

export interface ScriptVariablesMessage {
    t: MsgType.ScriptVariables;
    epoch: number;                // the server's; a report echoes the last one this connection brought (never 0)
    watch: ScriptVariableKey[];   // from the server's serve alone: the keys this seat reads once a second
    entries: ScriptVariableEntry[];
    served: boolean;              // true for the record served at a login
    own?: ScriptVariableEntry[];  // from the server alone: this player's own quests' state, applied first
}
