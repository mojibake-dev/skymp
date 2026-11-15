import { MsgType } from "../../messages";

// thuum D-53-8 B (train 53's V-live): the head parts this game draws on its
// own player after that player's race changed (the platform's
// ownHeadParts: its base's overlay list while the engine answers
// HasOverlays, else its base list). Client to server, reliable, R2: the
// server records them as the player's look when the race is the one it
// records and every part is an HDPT, and sends that look to every other game
// that sees the player (OwnLookService). Names no actor: only ever this
// game's own player.
export interface OwnHeadPartsMessage {
    t: MsgType.OwnHeadParts;
    race: number; // the race they are drawn under, as this game has it
    parts: number[]; // in the engine's order, each once, 1 to 64
    overlay: boolean; // the overlay list, else the base list
}
