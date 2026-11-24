import { MsgType } from "../../messages";

// A player's own game's answer to a look asked of it: whether its player
// sees `target` now, by its own engine's test. This game sends it at a
// watch's first read, at each change and at each ask; the server sends it
// on to the seat that hosts the target's cell, whose script asks, and that
// seat's platform answers the script from it (setPlayerLook). Both
// directions, reliable. The contract is skymp-wire's (wire-schema skymp.rs,
// PlayerLook, MsgType 89, wire id 97).
export interface PlayerLookMessage {
    t: MsgType.PlayerLook;
    // the looking player's actor, by its server id
    player: number;
    // the reference looked at, a load-order form
    target: number;
    // whether the player sees it
    los: boolean;
}
