import { MsgType } from "../../messages";

// thuum tracker #97, ADR-079 (docs/verbs/jail.md): the arrest this game's own
// engine started (GoToPrison), sent before the strip's moves so the server
// takes them. Client to server, reliable. The arrest is this game's
// (ADR-063): the server records the jail term, its end on the shared clock,
// permits the one jump to the faction's own jail landing and takes the strip
// into the faction's two chests and its jail outfit for the arrest's window.
// The contract is skymp-wire's (wire-schema skymp.rs, Arrest, MsgType 86).
export interface ArrestMessage {
    t: MsgType.Arrest;
    faction: number; // the crime faction that jailed this game's player, never 0
    days: number; // the sentence its engine set, 1 to 7 game days
}
