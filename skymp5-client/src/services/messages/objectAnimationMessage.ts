import { MsgType } from "../../messages";

// thuum docs/verbs/traps.md (ADR-059 decision 3): a world object's animation
// event as the cell's host's engine played it (a puzzle pillar's Turned01, a
// lever's FullPull, a portcullis's Open). The host sends it; the server
// records it as the reference's lastAnimation and relays it to the cell's
// other games, whose engines run no world script and play it. Reliable.
export interface ObjectAnimationMessage {
    t: MsgType.ObjectAnimation;
    refr: number; // the animated reference's form id, never 0
    event: string; // the behavior graph event name, never empty
}
