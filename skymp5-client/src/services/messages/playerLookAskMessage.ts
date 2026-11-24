import { MsgType } from "../../messages";

// A look asked of a player's own game: whether that player sees `target`,
// which only its own engine can tell. This game sends it when a script it
// runs for another player asked that player's figure (the platform's
// lookAsked); the server sends it on to the looking player's game alone,
// which watches its own player's look for 10 s (the wire's LOOK_WATCH_MS)
// and answers each change with PlayerLook. Both directions, reliable. The
// contract is skymp-wire's (wire-schema skymp.rs, PlayerLookAsk, MsgType 88,
// wire id 96).
export interface PlayerLookAskMessage {
    t: MsgType.PlayerLookAsk;
    // the looking player's actor, by its server id
    player: number;
    // the reference looked at, a load-order form
    target: number;
}
