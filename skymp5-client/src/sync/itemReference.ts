import { ownName } from "./itemName";

// An item's world reference that holds its own stack's rows (a dropped
// stack's copy: the server keeps the stack's extras on the copy, and every
// game that loads it hears them in its create word) shows what its stack is:
// the temper of the row of its own base, a health other than 1 (1 is the
// plain item, the engine's default for a placed one). A field left undefined
// stays as the engine holds it. Its name is the stack's own (sync/itemName.ts:
// a tempered stack's engine label is the engine's to add, from the health),
// and its charge the enchantment's charge the stack had left (an enchanted
// weapon's or a staff's ExtraCharge; a full one carries none)
export interface ItemReferenceLook {
  health?: number;
  name?: string;
  charge?: number;
}

export const itemReferenceLook = (
  entries: ReadonlyArray<{ baseId: number; health?: number; name?: string; chargePercent?: number }>,
  baseId: number,
  baseName: string,
): ItemReferenceLook => {
  const row = entries.find((e) => e.baseId === baseId);
  const look: ItemReferenceLook = {};
  if (row && typeof row.health === "number" && Number.isFinite(row.health) && Math.abs(row.health - 1) > 1e-6) {
    look.health = row.health;
  }
  if (row && typeof row.chargePercent === "number" && Number.isFinite(row.chargePercent) && row.chargePercent > 0) {
    look.charge = row.chargePercent;
  }
  const name = row ? ownName(row.name, row.health, baseName) : undefined;
  if (name !== undefined) {
    look.name = name;
  }
  return look;
};
