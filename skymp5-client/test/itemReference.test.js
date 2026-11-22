// What a dropped stack's world copy shows of its stack (src/sync/itemReference.ts),
// JS state alone: compiled by the package's own tsc into build/test and run
// with plain node (yarn test).
"use strict";
const assert = require("assert");
const { itemReferenceLook } = require("../build/test/itemReference.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

test("a tempered dagger's copy shows its temper", () => {
  const rows = [{ baseId: 0x1397e, count: 1, health: 1.100000023841858, name: "Iron Dagger (Fine)" }];
  assert.deepStrictEqual(itemReferenceLook(rows, 0x1397e), { health: 1.100000023841858 });
});

test("a plain copy, or one with no row of its base, changes nothing", () => {
  assert.deepStrictEqual(itemReferenceLook([{ baseId: 0x1397e, count: 1, health: 1 }], 0x1397e), {});
  assert.deepStrictEqual(itemReferenceLook([{ baseId: 0x1397e, count: 1 }], 0x1397e), {});
  assert.deepStrictEqual(itemReferenceLook([{ baseId: 0x3eadd, count: 1, health: 1.2 }], 0x1397e), {});
  assert.deepStrictEqual(itemReferenceLook([], 0x1397e), {});
});

let failed = 0;
for (const [name, fn] of cases) {
  try {
    fn();
    console.log(`ok - ${name}`);
  } catch (e) {
    failed += 1;
    console.log(`not ok - ${name}\n${e && e.stack ? e.stack : e}`);
  }
}
console.log(`${cases.length - failed} of ${cases.length} passed`);
process.exit(failed === 0 ? 0 : 1);
