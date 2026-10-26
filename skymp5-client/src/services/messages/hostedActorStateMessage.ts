import { MsgType } from "../../messages";

// thuum docs/verbs/npc-stream.md (ADR-050 decision 1, slice 2): the state of
// an actor this game simulates, as its engine has it: the three percentages
// (0 to 1), its combat target as the server knows it (a remote id, 0 for
// none) and the package its last package event named (0 for none). Client to
// server, under the actor's idx (sendMessageWithRefrId maps the remote id);
// at most once per 500 ms per hosted actor and only on a change; absolute,
// so a lost one is replaced by the next (unreliable), the first for an actor
// reliable (the keyframe after a grant). The server bounds the values by its
// own numbers (wire-rules hosted_state) and records the rest (R2); its output
// is ChangeValues to the actor's other listeners. No epoch: the client cannot
// name a persistent reference's cell, and the server's host table already
// refuses a sender that is not the actor's host (the verb doc's Q1, revised
// at the build).
export interface HostedActorStateMessage {
    t: MsgType.HostedActorState;
    idx: number;
    health: number;
    magicka: number;
    stamina: number;
    combatTarget: number;
    package: number;
}
