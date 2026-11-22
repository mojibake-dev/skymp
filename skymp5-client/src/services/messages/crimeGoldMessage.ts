import { MsgType } from "../../messages";

// One hold's bounty as this game's engine holds it (its crime gold for the
// faction, whole gold), sent when it changes. Client to server, reliable. The
// record is the server's (R0), told back at a login through
// Faction.SetCrimeGold and SetCrimeGoldViolent. The contract is skymp-wire's
// (wire-schema skymp.rs, CrimeGold, MsgType 87).
export interface CrimeGoldMessage {
    t: MsgType.CrimeGold;
    faction: number; // the crime faction, never 0
    violent: number; // its violent crime gold, 0 to 2147483647
    nonViolent: number; // its non-violent crime gold, 0 to 2147483647
    paidGold?: number; // the gold this game's engine paid as it set the bounty, its fine; never 0
}
