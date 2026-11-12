import { MsgType } from "../../messages";

// thuum docs/verbs/prop-physics.md (ADR-058; A1 to A7 of ADR-071 decision 2):
// who simulates a prop from now, from the server to every listener of the
// prop at each owner change (a grant, the return to the host at any owner's
// own asleep sample, a refused final sample, the host's own wake of a resting
// prop naming it by id, a departure, the simulator's unload of the cell), to
// one seat as the answer to its refused claim or sample, and to a late
// listener after the create word, reliable. The named player's seat
// simulates, every other seat's follows the stream; player 0 names the host
// of the cell the prop's record stands in; a word with an epoch below one
// already applied is dropped and an equal one replaces (one counter per
// prop). Ownership changes by this word alone: no seat infers it from a timer.
export interface PropOwnerMessage {
    t: MsgType.PropOwner;
    refr: number;
    player: number;
    epoch: number;
}
