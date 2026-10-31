import { MsgType } from "../../messages";

// thuum docs/verbs/spell-placed-object.md (ADR-057 shape A, D70): where this
// game's engine stands one base of a requested set. Client to server,
// reliable, once per base per set, R1: the server validates (the set its own
// request to this player and still open, the base one the set asked for and
// not yet reported, the pose 150 to 260 units from where the caster stood at
// the cast on the ground plane and within 256 of its height) and places its
// copy there for every other seat.
export interface PlacedReportMessage {
    t: MsgType.PlacedReport;
    set: number;
    base: number;
    pos: number[];
    yaw: number;
}
