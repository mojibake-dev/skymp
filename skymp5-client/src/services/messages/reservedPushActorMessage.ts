import { MsgType } from "../../messages";

// The wire's id contract (wire-schema SCHEMA_VERSION's note): MsgType 85 is
// thuum tracker #175's PushActor on fork branch m4-pt56-175-npc-shout, a dead
// placeholder here (ReservedPushActorMessage.h's pair). Never sent.
export interface ReservedPushActorMessage {
    t: MsgType.ReservedPushActor;
}
