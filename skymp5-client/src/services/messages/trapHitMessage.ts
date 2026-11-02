import { MsgType } from "../../messages";

// thuum docs/verbs/traps.md (ADR-059 decision 4): a trap's hit on another
// player's body as the cell's host's engine dealt it. The host sends it; the
// server checks the number against the trap's own table and the hit against
// the trap's reach, applies the table's value to the target's record and
// answers with ChangeValues. Reliable, client to server.
export interface TrapHitMessage {
    t: MsgType.TrapHit;
    refr: number; // the trap reference's form id, never 0
    target: number; // the hit actor's form id, another player's; never 0
    damage: number; // the damage the host's engine dealt; finite, 0 or above
}
