// Whether an actor's copy is down, so its movement record does not move it:
// its player's relayed ragdoll, a knock this game marked (sync/knockState.ts),
// or, for another player's figure, any ragdoll this copy's engine holds (a
// grab's or a throw's, which marks no knock). A record placing a ragdolled
// figure moved its root alone: a tail's bodies stayed behind, hundreds of
// units off its COM, and the figure stood torn. Its stream or its rest places
// it; the record resumes after its get-up. JS state alone.

export interface RecordDownFacts {
    // its player's relayed Ragdoll (Variable10 under -999)
    relayedRagdoll: boolean;
    // a knock this game marked, until its get-up
    knockedDown: boolean;
    // another player's figure, which the engine's ragdoll read below covers
    figure: boolean;
    // this copy's engine holds it ragdolled now (Actor::IsInRagdollState)
    ragdollHere: boolean;
}

export const recordDown = (f: RecordDownFacts): boolean => {
    return f.relayedRagdoll || f.knockedDown;
};
