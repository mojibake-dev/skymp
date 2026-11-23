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
// samples it no more, and the stream's rest records it
export function streamCarries(f: DeadBodyFacts): boolean {
    return f.held || f.streaming;
}

// a prop the rescan tracks for the first time is the engine's own body in
// place, with no copy being spawned and so no grace before its first tick:
// a knocked live body (its knock's first second is its flight)
export function trackedInPlace(live: boolean, deadBody: boolean, named: boolean): boolean {
    return live;
}
