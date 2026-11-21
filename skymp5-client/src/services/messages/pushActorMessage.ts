import { MsgType } from "../../messages";

// thuum tracker #175 (ADR-071 D-PH5's amendment, Eli's B: the NPC's host
// throws it): a script's PushActorAway in this game onto an actor another
// seat simulates (a puppet here), skipped by the platform's push hook and
// reported, so the actor's own simulator throws it and every other screen
// follows its stream. Client to server, reliable. The contract is
// skymp-wire's (wire-schema skymp.rs, PushActor, MsgType 85, wire id 93).
export interface PushActorMessage {
    t: MsgType.PushActor;
    // the pusher (the script's self) and the pushed actor, by server ids
    caster: number;
    target: number;
    // PushActorAway's force, from 0 to the wire's PUSH_FORCE_MAX (100)
    force: number;
}
