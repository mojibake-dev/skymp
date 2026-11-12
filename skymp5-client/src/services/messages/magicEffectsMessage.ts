import { MsgType } from "../../messages";

// The effects running on an actor (thuum docs/verbs/magic-effects.md). Server
// to client: whenever the set changes, and in full when this game first sees
// the actor; this game shows each on that actor for the seconds left, and
// for its own player holds the sums the running buffs put on its actor
// values (modifiers, by actor value; one left out is 0). The effects are the
// server's; each message replaces the actor's set, and arrives before the
// ChangeValues of the same change.
export interface MagicEffectsMessage {
    t: MsgType.MagicEffects;
    idx: number;
    effects: Array<{
        effect: number;
        source: number;
        magnitude: number;
        // seconds left; 0 for an effect without duration, a negative (the
        // server's -1) for a state with no clock, a thrall's reanimate: no end
        remaining: number;
    }>;
    modifiers?: Array<{
        av: number;
        amount: number;
    }>;
}
