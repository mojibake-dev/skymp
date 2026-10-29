import { MsgType } from "../../messages";

// thuum docs/verbs/prop-physics.md (ADR-058): who simulates a prop from now,
// from the server to every listener of the prop at each owner change,
// reliable. The named player's seat makes its body Dynamic with the last
// streamed velocity, every other seat's goes Keyframed; a word with an epoch
// below one already applied is dropped (one counter per prop).
export interface PropOwnerMessage {
    t: MsgType.PropOwner;
    refr: number;
    player: number;
    epoch: number;
}
