import { MsgType } from "../../messages";

// This game's player's jail term as the server's record holds it (R0): at
// the arrest's record, at every login while the term stands, and once when
// the shared clock passes its end. The end is a time on the shared clock,
// which runs while the player is away, so the time left is read from this
// rather than from this game's own clock, and a served term starts the
// release here. Server to client, reliable. The contract is skymp-wire's
// (wire-schema skymp.rs, JailTerm, MsgType 88).
export interface JailTermMessage {
    t: MsgType.JailTerm;
    faction: number; // the crime faction that jailed the player, never 0
    releaseAt: number; // the shared clock's days passed (GameDaysPassed) at the end, at least 0
    served: boolean; // the shared clock has passed the end
}
