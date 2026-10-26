import { MsgType } from "../../messages";

// thuum docs/verbs/npc-talk.md (ADR-052): this player asks for an NPC another
// game simulates, for a dialogue (claim true), or gives it back (claim
// false). Client to server, reliable, under the actor's idx
// (sendMessageWithRefrId maps the remote id); the server grants when this
// player stands in the actor's cell within activation reach and the actor is
// an NPC, alive, not in combat and unclaimed, and the word (isHostedByOther)
// then names this game as the simulator for the claim's lease (TalkService).
export interface ActorClaimMessage {
    t: MsgType.ActorClaim;
    idx: number;
    claim: boolean;
}
