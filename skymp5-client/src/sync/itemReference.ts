// An item's world reference that holds its own stack's rows (a dropped
// stack's copy: the server keeps the stack's extras on the copy, and every
// game that loads it hears them in its create word) shows what its stack is:
// the temper of the row of its own base, a health other than 1 (1 is the
// plain item, the engine's default for a placed one). A field left undefined
// stays as the engine holds it
export interface ItemReferenceLook {
  health?: number;
}

export const itemReferenceLook = (
  entries: ReadonlyArray<{ baseId: number; health?: number }>,
  baseId: number,
): ItemReferenceLook => {
  const row = entries.find((e) => e.baseId === baseId);
  const look: ItemReferenceLook = {};
  if (row && typeof row.health === "number" && Number.isFinite(row.health) && Math.abs(row.health - 1) > 1e-6) {
    look.health = row.health;
  }
  return look;
};
