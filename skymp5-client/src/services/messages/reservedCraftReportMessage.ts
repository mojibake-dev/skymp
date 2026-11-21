import { MsgType } from "../../messages";

// MsgType 84, reserved for CraftReport and never sent; the pair of
// ReservedCraftReportMessage.h.
export interface ReservedCraftReportMessage {
    t: MsgType.ReservedCraftReport;
}
