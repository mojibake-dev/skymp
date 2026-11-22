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
  // a take was named by this hold: its base stays held until its end, but
  // it names no other take
  taken?: boolean;
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
  // a hold held again for the server's echo keeps its take
  const before = holds.get(localId);
  const taken = before !== undefined && before.taken === true && before.baseId === baseId ? true : undefined;
  holds.delete(localId);
  if (holds.size >= holdsMax) {
    const oldest = holds.keys().next();
    if (!oldest.done) {
      holds.delete(oldest.value);
    }
  }
  holds.set(localId, { baseId, until, why, viewLogged: false, taken });
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
// a player's activation of a game-file item, asked of the server and not yet
// granted: its reference and base, read in the activation's own game context,
// and when it was asked; bounded, the oldest going first
const asked = new Map<number, { baseId: number; at: number }>();
const askedMax = 16;
export const notePickupAsked = (localId: number, baseId: number, now: number): void => {
  asked.delete(localId);
  if (asked.size >= askedMax) {
    const oldest = asked.keys().next();
    if (!oldest.done) {
      asked.delete(oldest.value);
    }
  }
  asked.set(localId, { baseId, at: now });
};

// the server's grant of `localId` to this game (its Activate(player, true)
// snippet, JS state alone): the asked pickup becomes a hold of its reference
// for PICKUP_HOLD_MS; false when nothing was asked of it or the ask lapsed,
// so a refused activation never holds
export const grantPickup = (localId: number, now: number): boolean => {
  const ask = asked.get(localId);
  asked.delete(localId);
  if (ask === undefined || now - ask.at > PICKUP_HOLD_MS) {
    return false;
  }
  holdBook(localId, ask.baseId, now + PICKUP_HOLD_MS, "its pickup's grant");
  return true;
};

// the reference a take of `baseId` into the player's inventory is this
// game's own, named once: the oldest hold still running at `now` of that base
// that named no take yet, else the oldest such pickup's hold of any base, one
// with an end (a leveled placement's engine may add the item it rolled under
// another base than the one the reference was read with; an open book's
// hold, which has no end, is matched by its base alone). The hold is marked
// taken; undefined for none
export const takeHeldReference = (baseId: number, now: number): number | undefined => {
  let sameBase: number | undefined;
  let oldestPickup: number | undefined;
  holds.forEach((held, localId) => {
    if (held.until <= now || held.taken === true) {
      return;
    }
    if (sameBase === undefined && held.baseId === baseId) {
      sameBase = localId;
    }
    if (oldestPickup === undefined && Number.isFinite(held.until)) {
      oldestPickup = localId;
    }
  });
  const named = sameBase ?? oldestPickup;
  if (named !== undefined) {
    const held = holds.get(named);
    if (held !== undefined) {
      held.taken = true;
    }
  }
  return named;
};

// the one entry a pickup reports out of the player's inventory difference
// (the record less the game: a gain is a negative count): the gain of the
// base the engine added, counted as that add at most; undefined for none.
// Every other difference is left to its own path
export const pickupTakeEntry = <E extends { baseId: number; count: number }>(diff: E[], baseId: number, numItems: number): E | undefined => {
  const entry = diff.find((e) => e.baseId === baseId && e.count < 0);
  if (entry === undefined) {
    return undefined;
  }
  const count = Math.min(-entry.count, Math.max(1, Math.floor(numItems)));
  return { ...entry, count: -count };
};
