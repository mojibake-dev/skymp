import { MsgType } from "../../messages";

// thuum docs/verbs/traps.md (M4): a linked activation the cell's host
// reported (a plate firing its spike wall, a lever opening its portcullis),
// for this game's engine to play with the caster the host's did. Server to
// client, reliable, the cell's other games.
export interface RelayedActivationMessage {
    t: MsgType.RelayedActivation;
    target: number; // the activated reference's form id, never 0
    caster: number; // the reference that activated it, never 0
    // thuum docs/DECISIONS.md D-C2 A (docs/research/fifty-fixes-2026-11-07.md
    // 1(a)): 1 OnTriggerEnter, 2 OnTriggerLeave: a guest's entry into a world
    // trigger, sent to the cell's host alone, whose engine sends that event to
    // the target's scripts with the caster (the presser) as akActionRef
    // instead of activating; absent or null for an activation
    entry?: number | null;
}
