import { MsgType } from "../../messages";

// thuum docs/verbs/prop-physics.md (ADR-058; A2 and A3 of ADR-071 decision
// 2; v2, ADR-071 decision 6): this player touched props another seat
// simulates. Client to server, reliable, under the references' form ids, one
// to sixteen, none 0, each once (an impact names every body it moved), each
// judged alone; touch is 0 for a contact of the character controller, 1 a
// grab (sent while this engine holds the body), 2 a drop (an item dropped, or
// a hold let go), 3 a shout's or spell's impulse or a trap body this seat's
// script set moving. The server arbitrates: a prop at rest or stalled is the
// newest toucher's (one epoch per prop) and every listener hears the word
// (PropOwner); a prop another seat streams awake is refused to a contact, a
// drop or an impulse, a grab takes it; a grabbed prop refuses everything; a
// refused claimant hears the current word.
export interface PropClaimMessage {
    t: MsgType.PropClaim;
    refrs: number[];
    touch: number;
}
