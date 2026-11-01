import { MsgType } from "../../messages";

// thuum docs/verbs/word-wall.md (M4): the word wall trigger this game fired.
// Client to server, reliable; the server reads the word from the wall's own
// record, checks the reach and writes it into the shout record.
export interface WordWallMessage {
    t: MsgType.WordWall;
    wall: number; // the trigger reference's form id, never 0
}
