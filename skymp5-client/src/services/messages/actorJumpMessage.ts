import { MsgType } from "../../messages";

// thuum docs/verbs/cast-teleport.md (the teleport snap): the actor at idx
// made a jump the server vouches for, so this game places its figure at the
// movement report that follows instead of gliding it there. From the server,
// reliable, just before that report.
export interface ActorJumpMessage {
    t: MsgType.ActorJump;
    idx: number;
}
