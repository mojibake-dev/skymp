// What the stream's no-row verdict (a body whose parts the wire cannot carry)
// writes into the kind cache: the verdict and its time, so no rescan takes the
// body again before it is judged anew. A living actor's body gets none: its
// kind is judged again at every read and no rescan takes a living actor, and
// a verdict written on an entry without the live-body marks reads "none" for
// good, so the next grip of a thrown player's figure would track nothing
export const noRowEntry = (live: boolean, now: number): { world: null; noRowAt: number } | undefined =>
  live ? undefined : { world: null, noRowAt: now };
