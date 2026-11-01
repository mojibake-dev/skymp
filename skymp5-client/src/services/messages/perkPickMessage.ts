import { MsgType } from "../../messages";

// thuum docs/verbs/perks.md (M5): the perk this game's Skills menu took, for
// the server to judge by the perk record's conditions and the points left.
// Client to server, reliable.
export interface PerkPickMessage {
    t: MsgType.PerkPick;
    perk: number; // the perk's form id, never 0
}
