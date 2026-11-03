import { MsgType } from "../../messages";

// thuum docs/verbs/prop-physics.md (ADR-058; ADR-063 decision 4): who
// simulates a prop from now, from the server to every listener of the prop at
// each owner change (a grant, the return to the host at any owner's own asleep
// sample, a grab past its cap, a refused final sample, the host's own wake of
// a resting prop naming it by id, a departure) and to one seat as the answer
// to its refused claim or sample, reliable. The named player's seat simulates,
// every other seat's follows the stream; player 0 names the cell's host by
// default; a word with an epoch below one already applied is dropped and an
// equal one replaces (one counter per prop). Ownership changes by this word
// alone: no seat infers it from a timer.
export interface PropOwnerMessage {
    t: MsgType.PropOwner;
    refr: number;
    player: number;
    epoch: number;
}
