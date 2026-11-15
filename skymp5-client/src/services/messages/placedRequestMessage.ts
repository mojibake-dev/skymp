import { MsgType } from "../../messages";

// thuum docs/verbs/spell-placed-object.md (ADR-057 shape A, D70): the server
// asks this game, at its accepted cast of a listed spell, where its engine
// stands the objects the spell's script placed: the set's id the reports
// name, the bases to find (form ids in the load order both ends share) and
// the window in milliseconds within which the reports are taken. Server to
// client, reliable, the caster alone. Since thuum ADR-073 decision 13 (#54)
// no client answers it: the spell channel carries a spell's placement from
// its PlaceAtMe on (ScriptCallService's kind "created"), and the server's
// listed set closes its window with no report, placing nothing, until its
// table goes.
export interface PlacedRequestMessage {
    t: MsgType.PlacedRequest;
    set: number;
    bases: number[];
    windowMs: number;
}
