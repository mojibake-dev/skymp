import { MsgType } from "../../messages";

// A player's own global variables (thuum docs/verbs/player-globals.md,
// ADR-029's A2). From the server after a login: the globals it keeps per
// player, to watch, and the player's recorded values, which this game sets.
// To the server: the watched globals whose values changed since the last
// report, naming none to watch. The latest value wins per global.
export interface PlayerGlobalsMessage {
    t: MsgType.PlayerGlobals;
    watch: number[];
    values: Array<{
        global: number;
        value: number;
    }>;
}
