import { MsgType } from "../../messages";

// thuum docs/verbs/cell-host.md (ADR-035): this game's loaded set, one cell
// at a time: the cell fully loaded (true) or gone from the loaded set
// (false). To the server, reliable and ordered, under its 32-then-8-a-second
// budget; a cell the server's grid does not give this player yields nothing.
export interface CellLoadedMessage {
    t: MsgType.CellLoaded;
    cell: number;
    loaded: boolean;
}
