import { MsgType } from "../../messages";

// thuum docs/verbs/prop-physics.md (ADR-058): this player touched a prop
// another seat simulates. Client to server, reliable, under the reference's
// form id; touch is 0 for a contact of the character controller, 1 a grab, 2 a
// drop, 3 a shout's or spell's impulse. The server names the newest toucher the
// simulator (one epoch per prop) and tells every listener of the prop
// (PropOwner); refused when this player is not in the prop's cell or another
// player holds it grabbed.
export interface PropClaimMessage {
    t: MsgType.PropClaim;
    refr: number;
    touch: number;
}
