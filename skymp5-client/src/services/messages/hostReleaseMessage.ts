import { MsgType } from "../../messages";

// thuum docs/verbs/cell-host.md (ADR-035): this game stops hosting the cell;
// epoch names the grant that ends. From the server, reliable; a release for
// a grant this game does not hold is ignored (HostService).
export interface HostReleaseMessage {
    t: MsgType.HostRelease;
    cell: number;
    epoch: number;
}
