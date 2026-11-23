// What a dropped stack's world copy shows of its stack (src/sync/itemReference.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { itemReferenceLook } from "../src/sync/itemReference";

test("a tempered dagger's copy shows its temper, its engine label left to the engine", () => {
  const rows = [{ baseId: 0x1397e, count: 1, health: 1.100000023841858, name: "Iron Dagger (Fine)" }];
  assert.deepStrictEqual(itemReferenceLook(rows, 0x1397e, "Iron Dagger"), { health: 1.100000023841858 });
});

test("an enchanted dagger the player named shows its name on the ground", () => {
  const rows = [{ baseId: 0x139ae, count: 1, name: "testEbony Dagger" }];
  assert.deepStrictEqual(itemReferenceLook(rows, 0x139ae, "Ebony Dagger"), { name: "testEbony Dagger" });
  const tempered = [{ baseId: 0x1397e, count: 1, health: 1.1, name: "Wolfbane (Fine)" }];
  assert.deepStrictEqual(itemReferenceLook(tempered, 0x1397e, "Iron Dagger"), { health: 1.1, name: "Wolfbane" });
});

test("a plain copy, or one with no row of its base, changes nothing", () => {
  const plain = [{ baseId: 0x1397e, count: 1, health: 1 }];
  const noHealth = [{ baseId: 0x1397e, count: 1 }];
  const otherBase = [{ baseId: 0x3eadd, count: 1, health: 1.2 }];
  assert.deepStrictEqual(itemReferenceLook(plain, 0x1397e, "Iron Dagger"), {});
  assert.deepStrictEqual(itemReferenceLook(noHealth, 0x1397e, "Iron Dagger"), {});
  assert.deepStrictEqual(itemReferenceLook(otherBase, 0x1397e, "Iron Dagger"), {});
  assert.deepStrictEqual(itemReferenceLook([], 0x1397e, "Iron Dagger"), {});
});
