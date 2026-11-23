// An inventory's entries and the difference of two inventories, from JS
// state alone: the part of inventory.ts that needs no engine, so a test
// can reach the apply's own compare.
import { CreatedForm, sameCreated } from "./createdFormKey";

export interface Extra {
  health?: number;
  enchantmentId?: number;
  maxCharge?: number;
  removeEnchantmentOnUnequip?: boolean;
  chargePercent?: number;
  name?: string;
  soul?: 0 | 1 | 2 | 3 | 4 | 5;
  poisonId?: number;
  poisonCount?: number;
  worn?: boolean;
  wornLeft?: boolean;
  // on the wire alone, a created form by its definition (sync/createdForms.ts):
  // the brewed potion or poison this stack is (then baseId is its game-file
  // template), the item's made enchantment (no enchantmentId then), a brewed
  // poison on the weapon (no poisonId then). An entry this engine reads names
  // its own forms by id instead
  createdBase?: CreatedForm;
  createdEnchantment?: CreatedForm;
  createdPoison?: CreatedForm;
  // the stack's owner, the engine's stolen mark (ExtraOwnership), as this
  // game holds it
  ownerId?: number;
}

export interface BasicEntry {
  baseId: number;
  count: number;
}

export type Entry = BasicEntry & Extra;

export interface Inventory {
  entries: Entry[];
}

// 'loxsword (Legendary)' => 'loxsword'
// The record holds an item's health as the wire's f32 (Inventory.h,
// skymp.rs), so 1.1 comes back as 1.100000023841858, while extractExtraData
// reads the engine's to one decimal. Health is compared at that one decimal on
// both sides, so a tempered stack the record holds is the stack the engine
// holds, and an apply does not remove and re-add it
export const sameHealth = (x: number | undefined, y: number | undefined): boolean =>
  x === undefined || y === undefined ? x === y : Math.round(x * 10) === Math.round(y * 10);

export const extrasEqual = (a: Entry, b: Entry, ignoreWorn = false) => {
  return (
    sameHealth(a.health, b.health) &&
    a.enchantmentId === b.enchantmentId &&
    a.maxCharge === b.maxCharge &&
    !!a.removeEnchantmentOnUnequip === !!b.removeEnchantmentOnUnequip &&
    //a.chargePercent === b.chargePercent &&
    //namesEqual(a, b) &&
    a.soul === b.soul &&
    a.poisonId === b.poisonId &&
    a.poisonCount === b.poisonCount &&
    // a wire entry's created forms by their definition
    sameCreated(a.createdBase, b.createdBase) &&
    sameCreated(a.createdEnchantment, b.createdEnchantment) &&
    sameCreated(a.createdPoison, b.createdPoison) &&
    a.ownerId === b.ownerId &&
    ((!!a.worn === !!b.worn && !!a.wornLeft === !!b.wornLeft) || ignoreWorn)
  );
};

export const hasExtras = (e: Entry): boolean => {
  return !extrasEqual(e, { baseId: 0, count: 0 });
};

// lhs less rhs: each of rhs's entries takes its count from lhs's entry of the
// same base and equal extras, or enters negated when lhs holds none; with
// ignoreWorn the worn flags are left out of the match. An apply's diff is the
// record less the game: a positive count is added, a negative one removed
export const getDiff = (
  lhs: Inventory,
  rhs: Inventory,
  ignoreWorn: boolean
): Inventory => {
  const lhsCopy: Inventory = JSON.parse(JSON.stringify(lhs));
  const rhsCopy: Inventory = JSON.parse(JSON.stringify(rhs));

  rhsCopy.entries.forEach((e) => {
    const sameFromLeft = lhsCopy.entries.find(
      (x) => x.baseId === e.baseId && extrasEqual(x, e, ignoreWorn)
    );
    if (sameFromLeft) {
      sameFromLeft.count -= e.count;
    } else {
      lhsCopy.entries.push(e);
      lhsCopy.entries[lhsCopy.entries.length - 1].count *= -1;
    }
  });

  return { entries: lhsCopy.entries.filter((x) => x.count !== 0) };
};
