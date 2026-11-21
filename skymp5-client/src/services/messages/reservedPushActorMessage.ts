import { MsgType } from "../../messages";

// MsgType 85, reserved for PushActor and never sent; the pair of
// ReservedPushActorMessage.h.
export interface ReservedPushActorMessage {
    t: MsgType.ReservedPushActor;
}
