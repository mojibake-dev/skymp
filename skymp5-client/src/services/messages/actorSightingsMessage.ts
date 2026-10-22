import { MsgType } from "../../messages";

// thuum docs/verbs/npc-puppet.md (ADR-045's revision of 2026-10-22; TES3MP's
// actor list): a host's roster of the game-file actors its engine has
// attached that the server holds no record of, with where the engine has
// them. The server loads each record on demand and places it there if this
// game hosts the cell of that position (PartOne::OnActorSighting). At most
// 64 per message (wire-validate's bound), every 2 s while any remain.
export interface ActorSighting {
    refrId: number;
    worldOrCell: number;
    pos: [number, number, number];
    rot: [number, number, number];
}

export interface ActorSightingsMessage {
    t: MsgType.ActorSightings;
    sightings: ActorSighting[];
}
