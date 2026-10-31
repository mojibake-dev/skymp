import { MsgType } from "../../messages";

// thuum docs/verbs/perks.md (M5): the server's whole perk set and the points
// left for this player: at login, after each pick (accepted or refused),
// after a level change and after the console's AddPerk, RemovePerk or
// SetLevel. Server to client, reliable.
export interface PerkStateMessage {
    t: MsgType.PerkState;
    perks: number[]; // the perks held, none 0, at most 512
    points: number; // the points left to spend
}
