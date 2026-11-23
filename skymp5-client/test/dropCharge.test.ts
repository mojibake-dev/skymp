// The charge a dropped stack keeps (src/sync/dropCharge.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { dropCharge } from "../src/sync/dropCharge";

// Skyrim.esm (thuum lab/esm.py): the Staff of Flames' enchantment holds 500 when full
test("a half-spent Staff of Flames dropped keeps the charge it had left", () => {
  assert.strictEqual(dropCharge(350, 500), 350);
  assert.strictEqual(dropCharge(0, 500), 0);
});

test("a full staff, or an item with no enchantment's charge, carries no charge", () => {
  assert.strictEqual(dropCharge(500, 500), undefined);
  assert.strictEqual(dropCharge(0, 0), undefined);
  assert.strictEqual(dropCharge(Number.NaN, 500), undefined);
});
