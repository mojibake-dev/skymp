import { MsgType } from "../../messages";

// thuum docs/verbs/npc-fate.md (ADR-048's B): this game's report that a
// game-file actor it simulates died of something the server has no number
// for. Client to server, reliable, once per death. The server takes it only
// when it can check the cause against what it saw (ActionListener::
// OnActorDeath): an actor's hit it numbered, a drop in this game's samples,
// an activation beside the body. Causes: 0 unknown (the server picks any
// evidence it has), 1 fall, 2 hazard, 3 script, 4 an actor's hit (the
// killer named). This game reads 4 from the death event's killer and sends
// 0 otherwise (DeathReportService).
export interface ActorDeathMessage {
    t: MsgType.ActorDeath;
    refrId: number;
    cause: number;
    killer: number;
    pos: [number, number, number];
}
