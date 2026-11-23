// Which path carries a dead NPC's body from the seat that simulates it: the
// prop stream (PropService: the root and the bones, followed on every other
// seat) or the death settle (sendInputsService: root positions alone, which
// a watcher does not apply to a dead copy). The facts are read by the
// caller; the choice is JS state alone.

export interface DeadBodyFacts {
    // a hand on the body here
    held: boolean;
    // the body's prop stream is open here
    streaming: boolean;
    // the prop row's word names this seat: the server's grant of the body
    // (the death's, a touch's)
    named: boolean;
}

// the prop stream carries the body: the death settle hands it over and
// samples it no more, and the stream's rest records it. The death's word
// names the seat that simulated the actor, so its fall streams from the
// word, root and bones, as a live body's knock does
export function streamCarries(f: DeadBodyFacts): boolean {
    return f.held || f.streaming || f.named;
}

// a prop the rescan tracks for the first time is the engine's own body in
// place, with no copy being spawned and so no grace before its first tick:
// a knocked live body, and a dead body whose word names this seat (each
// one's first second is its flight or its fall)
export function trackedInPlace(live: boolean, deadBody: boolean, named: boolean): boolean {
    return live || (deadBody && named);
}
