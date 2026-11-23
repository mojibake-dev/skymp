// What a dropped stack's world copy shows of its stack (src/sync/itemReference.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { itemReferenceLook } from "../src/sync/itemReference";

test("a tempered dagger's copy shows its temper", () => {
  const rows = [{ baseId: 0x1397e, count: 1, health: 1.100000023841858, name: "Iron Dagger (Fine)" }];
  assert.deepStrictEqual(itemReferenceLook(rows, 0x1397e), { health: 1.100000023841858 });
});

test("a plain copy, or one with no row of its base, changes nothing", () => {
  const plain = [{ baseId: 0x1397e, count: 1, health: 1 }];
  const noHealth = [{ baseId: 0x1397e, count: 1 }];
  const otherBase = [{ baseId: 0x3eadd, count: 1, health: 1.2 }];
  assert.deepStrictEqual(itemReferenceLook(plain, 0x1397e), {});
  assert.deepStrictEqual(itemReferenceLook(noHealth, 0x1397e), {});
  assert.deepStrictEqual(itemReferenceLook(otherBase, 0x1397e), {});
  assert.deepStrictEqual(itemReferenceLook([], 0x1397e), {});
});
