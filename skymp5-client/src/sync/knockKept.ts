// Whether the rescan keeps a body this seat's engine knocked moments ago (its
// own knock edge, or the server's relayed push onto an actor this seat
// hosts) while its stream's first sample is still to come. Its host owns the
// body already, so its row carries no claim, and the rescan keeps only a row
// near the player or one kept anywhere (owned by a word, streaming, held,
// followed, claimed moments ago). JS state alone.
export const KNOCK_KEEP_MS = 2000;

// knockedAt: when the knock tracked the body, 0 for a body no knock tracked
export const knockKept = (_knockedAt: number, _now: number): boolean => false;
