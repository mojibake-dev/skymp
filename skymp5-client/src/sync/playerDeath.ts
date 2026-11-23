// The server's death word for this game's own player beside the engine's
// killmove on it. The client's death of its player blocks every animation
// event on the player but Ragdoll and pushes the player into a ragdoll
// (DeathService.killActor); landing within a frame of the lethal hit, under
// a killmove's paired animation, it blanks the killmove's own events and
// ragdolls a body still in the paired pose. JS state alone.

export interface PlayerDeathFacts {
    // the engine plays a killmove on the player (Actor.IsInKillMove)
    inKillMove: boolean;
    // since the death word came
    waitedMs: number;
    // the longest a killmove holds the death off
    maxWaitMs: number;
}

// "wait" while the killmove plays, "die" to apply the death now
export function playerDeathStep(f: PlayerDeathFacts): "wait" | "die" {
    return f.inKillMove && f.waitedMs < f.maxWaitMs ? "wait" : "die";
}
