import { MsgType } from "../../messages";

// The player waited or slept, for the game hours the engine counted across
// the Sleep/Wait menu (thuum docs/verbs/rest.md). The server checks it and
// computes the recovery; the shared clock does not move.
export interface RestIntentMessage {
    t: MsgType.RestIntent;
    hours: number;
    sleep: boolean; // a sleep in a bed rather than a wait
}
