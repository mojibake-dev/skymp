// The pickup's holds (src/sync/bookHold.ts), JS state alone: compiled by the
// package's own tsc into build/test and run with plain node (yarn test).
"use strict";
const assert = require("assert");
const h = require("../build/test/bookHold.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

test("two granted pickups of one base are named oldest first, each once", () => {
  const t = 1000000;
  h.notePickupAsked(0x1001, 0x13986, t);
  h.notePickupAsked(0x1002, 0x13986, t + 1);
  assert.strictEqual(h.grantPickup(0x1001, t + 2), true);
  assert.strictEqual(h.grantPickup(0x1002, t + 3), true);
  assert.strictEqual(h.takeHeldReference(0x13986, t + 4), 0x1001);
  assert.strictEqual(h.takeHeldReference(0x13986, t + 5), 0x1002);
  assert.strictEqual(h.takeHeldReference(0x13986, t + 6), undefined);
  // both bases stay held against the inventory apply until their end
  assert.ok(h.heldBookBases(t + 7).includes(0x13986));
});

test("a refused activation, never granted, leaves no hold", () => {
  const t = 2000000;
  h.notePickupAsked(0x2001, 0x31941, t);
  assert.strictEqual(h.takeHeldReference(0x31941, t + 1), undefined);
  assert.ok(!h.heldBookBases(t + 1).includes(0x31941));
  // a grant after the ask lapsed holds nothing either
  assert.strictEqual(h.grantPickup(0x2001, t + h.PICKUP_HOLD_MS + 1), false);
  assert.strictEqual(h.takeHeldReference(0x31941, t + h.PICKUP_HOLD_MS + 2), undefined);
});

test("a grant nothing asked for, a door's walk-through, holds nothing", () => {
  assert.strictEqual(h.grantPickup(0x3001, 3000000), false);
});

test("a leveled placement's roll under another base falls back to the oldest pickup", () => {
  const t = 4000000;
  h.notePickupAsked(0x4001, 0x6a0b9, t);
  assert.strictEqual(h.grantPickup(0x4001, t + 1), true);
  assert.strictEqual(h.takeHeldReference(0x12eb7, t + 2), 0x4001);
  assert.strictEqual(h.takeHeldReference(0x12eb7, t + 3), undefined);
});

test("an open book's hold is named by its base alone, never by the fallback", () => {
  const t = 5000000;
  h.holdBook(0x5001, 0xed046, Infinity, "while its Book Menu decides");
  assert.strictEqual(h.takeHeldReference(0x99999, t), undefined);
  assert.strictEqual(h.takeHeldReference(0xed046, t + 1), 0x5001);
  // held again for the server's echo, it names no second take
  h.holdBook(0x5001, 0xed046, t + 10000, "until the server's record of its Take comes back");
  assert.strictEqual(h.takeHeldReference(0xed046, t + 2), undefined);
  h.releaseBook(0x5001);
});

test("a pickup reports the one entry of the base the engine added, the rest left alone", () => {
  const diff = [
    { baseId: 0x13986, count: -1, ownerId: 0x5a666 },
    { baseId: 0xf, count: 40 },
    { baseId: 0x1397e, count: -5 },
  ];
  assert.deepStrictEqual(h.pickupTakeEntry(diff, 0x13986, 1), { baseId: 0x13986, count: -1, ownerId: 0x5a666 });
  assert.strictEqual(h.pickupTakeEntry(diff, 0x12345, 1), undefined);
  // a gain only: a loss of that base is no take
  assert.strictEqual(h.pickupTakeEntry(diff, 0xf, 40), undefined);
  // counted as the engine's add at most
  assert.deepStrictEqual(h.pickupTakeEntry(diff, 0x1397e, 2), { baseId: 0x1397e, count: -2 });
  assert.strictEqual(diff.length, 3);
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
