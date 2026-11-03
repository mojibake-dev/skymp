import { MsgType } from "../../messages";

// thuum docs/verbs/prop-physics.md (ADR-058): this player touched a prop
// another seat simulates. Client to server, reliable, under the reference's
// form id; touch is 0 for a contact of the character controller, 1 a grab, 2 a
// drop, 3 a shout's or spell's impulse. The server arbitrates (ADR-063
// decision 4): a prop at rest or stalled is the newest toucher's (one epoch
// per prop) and every listener hears the word (PropOwner); a prop another seat
// streams awake is refused to a contact, a drop or an impulse, a grab takes
// it; a grabbed prop refuses everything.
export interface PropClaimMessage {
    t: MsgType.PropClaim;
    refr: number;
    touch: number;
}
