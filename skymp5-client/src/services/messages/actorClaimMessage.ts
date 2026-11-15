import { MsgType } from "../../messages";

// thuum docs/verbs/npc-talk.md (ADR-052): this player asks for an NPC another
// game simulates, for a dialogue (claim true), or gives it back (claim
// false). Client to server, reliable, under the actor's idx
// (sendMessageWithRefrId maps the remote id); the server grants when this
// player stands in the actor's cell within activation reach and the actor is
// an NPC, alive, not in combat and unclaimed, and the word (isHostedByOther)
// then names this game as the simulator for the claim's lease (TalkService).
//
// Since SCHEMA_VERSION 47 (thuum ADR-073 decision 6, step 5a) a claim or a
// release names its purpose: absent or 0 a talk as before, 1 a quest's lead
// (`quest` and `package`: the quest's own package this game runs on the actor,
// which fills one of the quest's aliases; D-53-4), 2 a scene this game
// started (`quest` and `scene`; D-L2 B). The server reads the records and
// finds the alias itself.
export interface ActorClaimMessage {
    t: MsgType.ActorClaim;
    idx: number;
    claim: boolean;
    purpose?: number;
    quest?: number;
    package?: number;
    scene?: number;
}
