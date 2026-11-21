import { MsgType } from "../../messages";

// The wire's id contract (wire-schema SCHEMA_VERSION's note): MsgType 84 is
// thuum tracker #98's CraftReport on fork branch m4-pt56-98, a dead
// placeholder here (ReservedCraftReportMessage.h's pair). Never sent.
export interface ReservedCraftReportMessage {
    t: MsgType.ReservedCraftReport;
}
