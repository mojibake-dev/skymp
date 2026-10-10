import { MsgType } from "../../messages";

// The effects running on an actor (thuum docs/verbs/magic-effects.md). Server
// to client: whenever the set changes, and in full when this game first sees
// the actor; this game shows each on that actor for the seconds left. The
// effects are the server's; each message replaces the actor's set.
export interface MagicEffectsMessage {
    t: MsgType.MagicEffects;
    idx: number;
    effects: Array<{
        effect: number;
        source: number;
        magnitude: number;
        remaining: number;
    }>;
}
