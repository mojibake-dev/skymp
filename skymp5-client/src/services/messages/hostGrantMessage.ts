import { MsgType } from "../../messages";

// thuum docs/verbs/cell-host.md (ADR-035): this game hosts the cell from now,
// at this epoch (one counter per cell, up at every grant). From the server,
// reliable; a grant whose epoch is not above the last seen for the cell in
// this connection is ignored (HostService).
export interface HostGrantMessage {
    t: MsgType.HostGrant;
    cell: number;
    epoch: number;
}
