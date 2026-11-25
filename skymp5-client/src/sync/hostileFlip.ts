// Whether a hosted actor's relayed combat target turned to this seat's player or away from it (PuppetService's
// noteRelayedTarget), JS state alone: true when it now names this player, false when it named this player and no
// longer does, undefined for any other change. The platform's registry holds the puppets whose host targets this
// player, and the compass shows them (PuppetDetection.h CompassAdds).
export function hostileFlip(before: number, target: number, me: number): boolean | undefined {
    void before;
    void target;
    void me;
    return undefined;
}
