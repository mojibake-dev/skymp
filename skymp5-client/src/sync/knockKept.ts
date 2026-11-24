// Whether the rescan keeps a body this seat's engine knocked moments ago (its
// own knock edge, or the server's relayed push onto an actor this seat
// hosts) while its stream's first sample is still to come. Its host owns the
// body already, so its row carries no claim, and the rescan keeps only a row
// near the player or one kept anywhere (owned by a word, streaming, held,
// followed, claimed moments ago). A rescan that fell between the knock's
// track and that first sample found the body past TRACK_RADIUS and untracked
// it as out of reach, so its throw streamed to no other seat until something
// knocked it again. Kept for as long as a claim awaits its word
// (CLAIM_EVERY_MS); its first sample makes it a streaming row, kept as one.
// JS state alone.
export const KNOCK_KEEP_MS = 2000;

// trackedAt: when the knock tracked the body, 0 for a body no knock tracked
export const knockKept = (trackedAt: number, now: number): boolean =>
  trackedAt !== 0 && now - trackedAt < KNOCK_KEEP_MS;
