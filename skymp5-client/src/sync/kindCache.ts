// What the stream's no-row verdict (a body whose parts the wire cannot carry)
// writes into the kind cache, and how long a living actor's verdict holds.
// The verdict and its time, so no rescan or grab takes the body again before
// it is judged anew. A living actor's carries the live-body marks: a verdict
// on an entry without them was never judged again, so the next grip of a
// thrown player's figure tracked nothing for good; and with no verdict at all
// a figure torn past the wire's reach was tracked and dropped every frame the
// grab named it. Held for a window (liveNoRowHeld), it is judged again after,
// so a grip of it once whole holds
export type NoRowEntry = { world: null; noRowAt: number; body?: true; live?: true };

export const noRowEntry = (live: boolean, now: number): NoRowEntry =>
  live ? { world: null, noRowAt: now, body: true, live: true } : { world: null, noRowAt: now };

export const liveNoRowHeld = (noRowAt: number | undefined, now: number, holdMs: number): boolean =>
  noRowAt !== undefined && now - noRowAt < holdMs;
