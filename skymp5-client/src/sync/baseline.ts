// The record as this game last heard it (containersService's lastInv), moved
// on by the differences one container event reported. The engine raises one
// event per stack it moves, and each event reports every difference between
// the baseline and the game's inventory, so a burst (a take-all, a strip)
// reports each stack again in every later event unless the baseline takes it
// in at once. Each reported entry is a stack's change, a put out of the
// player positive and a take into it negative; it moves the baseline's stack
// of the same base and extras (sameStack), or, a take of a stack the baseline
// has none of, adds the game's. Stacks with extras (an owner, a worn flag, an
// enchantment, a name) move as plain ones do
export const advanceBaseline = <E extends { baseId: number; count: number }>(
  baseline: E[],
  reported: E[],
  sameStack: (a: E, b: E) => boolean,
): E[] => {
  const out: E[] = baseline.map((e) => ({ ...e }));
  for (const r of reported) {
    if (r.count === 0) {
      continue;
    }
    const match = out.find((e) => sameStack(e, r));
    if (match) {
      match.count -= r.count;
    } else {
      out.push({ ...r, count: -r.count });
    }
  }
  return out.filter((e) => e.count > 0);
};
