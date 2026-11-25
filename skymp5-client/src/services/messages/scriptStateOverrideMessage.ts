import { MsgType } from "../../messages";

// One variable a script override sets, named as the type's chain declares it
// (an auto property's backing variable, "::wordLearned_var"), matched without
// case; `k` 3 an int, 4 a float, 5 a bool, with that kind's one field.
export interface ScriptStateOverrideVar {
    name: string;
    k: number;
    i?: number;
    f?: number;
    b?: boolean;
}

// One attached script an override names: its type, the state it goes back to
// ("" the empty state) and the variables it sets.
export interface ScriptStateOverrideScript {
    type: string;
    state: string;
    vars: ScriptStateOverrideVar[];
}

// The state of a world object's scripts that a player's own record decides,
// put back by name on this game's copy, which this game runs as the object's
// owner, ahead of the relayed entry that runs the scripts for that player on
// the same channel. Server to this game alone, reliable. The contract is
// skymp-wire's (wire-schema skymp.rs, ScriptStateOverride, MsgType 89).
export interface ScriptStateOverrideMessage {
    t: MsgType.ScriptStateOverride;
    refrId: number; // a reference of the shared load order, never 0
    scripts: ScriptStateOverrideScript[]; // 1 to 16, each type once
}
