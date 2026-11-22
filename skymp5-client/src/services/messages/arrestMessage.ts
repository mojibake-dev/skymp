import { MsgType } from "../../messages";

// The arrest this game's own engine started (GoToPrison), sent before the
// strip's moves so the server takes them. Client to server, reliable. The
// arrest is this game's own: the server records the jail term and its end on
// the shared clock, and takes the strip into the faction's two chests and its
// jail outfit for the arrest's window. The contract is skymp-wire's
// (wire-schema skymp.rs, Arrest, MsgType 86).
export interface ArrestMessage {
    t: MsgType.Arrest;
    faction: number; // the crime faction that jailed this game's player, never 0
    days: number; // the sentence its engine set, 1 to 7 game days
}
