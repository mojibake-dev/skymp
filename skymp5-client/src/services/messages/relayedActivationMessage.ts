import { MsgType } from "../../messages";

// thuum docs/verbs/traps.md (M4): a linked activation the cell's host
// reported (a plate firing its spike wall, a lever opening its portcullis),
// for this game's engine to play with the caster the host's did. Server to
// client, reliable, the cell's other games.
export interface RelayedActivationMessage {
    t: MsgType.RelayedActivation;
    target: number; // the activated reference's form id, never 0
    caster: number; // the reference that activated it, never 0
}
