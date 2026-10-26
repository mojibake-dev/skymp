import { MsgType } from "../../messages";

// thuum docs/verbs/npc-stream.md (the voice slice, ADR-050): a spoken line
// begins on an actor this game simulates: the actor (under its idx), the
// topic's form id and the topic info's (the line). Client to server,
// unreliable (a lost line is a lost line, TES3MP's shape); the server gates
// the host and relays it to the actor's other listeners, whose puppets say
// the same info through the engine's own say (VoiceService).
export interface ActorSpeechMessage {
    t: MsgType.ActorSpeech;
    idx: number;
    topic: number;
    info: number;
}
