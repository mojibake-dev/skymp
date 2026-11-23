// The pickup's holds and its take (src/sync/bookHold.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import * as h from "../src/sync/bookHold";

type Row = { baseId: number; count: number; health?: number; ownerId?: number };

test("a granted pickup holds its reference and base until its end", () => {
  const t = 1000000;
  h.notePickupAsked(0x1001, 0x13986, t);
  h.notePickupAsked(0x1002, 0x13986, t + 1);
  assert.strictEqual(h.grantPickup(0x1001, t + 2), true);
  assert.strictEqual(h.grantPickup(0x1002, t + 3), true);
  assert.strictEqual(h.bookHeld(0x1001, t + 4)?.baseId, 0x13986);
  assert.strictEqual(h.bookHeld(0x1002, t + 4)?.baseId, 0x13986);
  assert.ok(h.heldBookBases(t + 5).includes(0x13986));
  assert.strictEqual(h.bookHeld(0x1001, t + 2 + h.PICKUP_HOLD_MS), undefined);
});

test("a refused activation, never granted, leaves no hold", () => {
  const t = 2000000;
  h.notePickupAsked(0x2001, 0x31941, t);
  assert.strictEqual(h.bookHeld(0x2001, t + 1), undefined);
  assert.ok(!h.heldBookBases(t + 1).includes(0x31941));
  // a grant after the ask lapsed holds nothing either
  assert.strictEqual(h.grantPickup(0x2001, t + h.PICKUP_HOLD_MS + 1), false);
  assert.strictEqual(h.bookHeld(0x2001, t + h.PICKUP_HOLD_MS + 2), undefined);
});

test("a grant nothing asked for, a door's walk-through, holds nothing", () => {
  assert.strictEqual(h.grantPickup(0x3001, 3000000), false);
});

test("a pickup's take is the event's base, count and owner, the stack's extras from the difference", () => {
  const diff: Row[] = [
    { baseId: 0x13986, count: -1, health: 1.1 },
    { baseId: 0xf, count: 40 },
  ];
  assert.deepStrictEqual(
    h.pickupTake(diff, { baseId: 0x13986, count: 1, ownerId: 0x5a666 }),
    { baseId: 0x13986, count: 1, health: 1.1, ownerId: 0x5a666 },
  );
  assert.strictEqual(diff.length, 2);
  assert.strictEqual(diff[0].count, -1);
});

test("a pickup with no gain of its base in the difference is the event alone", () => {
  const diff: Row[] = [{ baseId: 0xf, count: 40 }];
  assert.deepStrictEqual(h.pickupTake(diff, { baseId: 0x12eb7, count: 1, ownerId: 0 }), { baseId: 0x12eb7, count: 1 });
  assert.deepStrictEqual(h.pickupTake([] as Row[], { baseId: 0xed046, count: 1, ownerId: 0x35d42 }), { baseId: 0xed046, count: 1, ownerId: 0x35d42 });
});

test("a stack's pickup reports the count its inventory gained", () => {
  const diff: Row[] = [{ baseId: 0xf, count: -25 }];
  assert.deepStrictEqual(h.pickupTake(diff, { baseId: 0xf, count: 25, ownerId: 0 }), { baseId: 0xf, count: 25 });
});

test("a pickup reports the one entry of the base the engine added, the rest left alone", () => {
  const diff: Row[] = [
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
