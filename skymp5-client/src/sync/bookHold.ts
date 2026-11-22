// thuum playtest 52 break 3 (the books client half). A world book whose Book
// Menu this game opened on the server's open reply is decided at the menu's
// close by two facts read from the engine (RemoteServer's book branch,
// services/services/remoteServer.ts): the reference gone from the world and
// the player holding more of its base than before the open. Two loops would
// erase them first: the player inventory apply (remoteServer.ts, every 5 s)
// takes back a copy the server's record does not hold, and a form view's
// harvested apply (modelApplyUtils.ts, every 666 ms) enables an item the
// record does not count as taken (the 10 s record x-book-take's leg A keeps
// asks both). So an open book holds its base against the first (the engine's
// surplus kept, inventory.ts keepSurplus) and its reference against the
// second, from the open until its close is decided, and after a Take for the
// server's echo of the pickup (its SetInventory and the harvested word), one
// hold a book. JS state alone, read by the two loops.
export interface BookHold {
  baseId: number;
  until: number;
  // why it holds, for the view's line
  why: string;
  // the view's line for this hold, once
  viewLogged: boolean;
}

// by the reference's local id; bounded, the oldest hold going first (the map
// keeps insertion order)
const holds = new Map<number, BookHold>();
const holdsMax = 16;

// how long a world item's pickup holds its reference and base: from the
// player's activation, through the server's grant and the engine's take, to
// the server's echo of the record (the grant lasts as long)
export const PICKUP_HOLD_MS = 10000;
export const holdBook = (localId: number, baseId: number, until: number, why: string): void => {
  holds.delete(localId);
  if (holds.size >= holdsMax) {
    const oldest = holds.keys().next();
    if (!oldest.done) {
      holds.delete(oldest.value);
    }
  }
  holds.set(localId, { baseId, until, why, viewLogged: false });
};

export const releaseBook = (localId: number): void => {
  holds.delete(localId);
};

// the reference's hold still running at `now`, a lapsed one dropped
export const bookHeld = (localId: number, now: number): BookHold | undefined => {
  const held = holds.get(localId);
  if (held !== undefined && held.until <= now) {
    holds.delete(localId);
    return undefined;
  }
  return held;
};

// the bases of the holds still running at `now`, the lapsed ones dropped
export const heldBookBases = (now: number): number[] => {
  const bases: number[] = [];
  holds.forEach((held, localId) => {
    if (held.until <= now) {
      holds.delete(localId);
      return;
    }
    bases.push(held.baseId);
  });
  return bases;
};
// the reference a take of `baseId` into the player's inventory is this game's
// own: the newest hold still running at `now` of that base, else the newest
// hold of any base (a leveled placement's engine may add the item it rolled
// under another base than the one the reference was read with); undefined
// for none
export const heldReferenceFor = (baseId: number, now: number): number | undefined => {
  let sameBase: number | undefined;
  let newest: number | undefined;
  holds.forEach((held, localId) => {
    if (held.until <= now) {
      return;
    }
    newest = localId;
    if (held.baseId === baseId) {
      sameBase = localId;
    }
  });
  return sameBase ?? newest;
};
