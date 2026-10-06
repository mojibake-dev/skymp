import { MsgType } from "../../messages";

// A player's RaceMenu look, as RaceMenu saves it (thuum
// docs/verbs/racemenu-sync.md). The client sends its own (actor 0) after the
// race menu closed and the look changed; the server sends a player's look,
// naming it by its server id, after a login and with that player's figure.
export interface RaceMenuPresetMessage {
    t: MsgType.RaceMenuPreset;
    actor: number; // 0 from a client; the player's server id from the server
    preset: string; // RaceMenu's JSON
}
