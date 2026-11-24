// A kept last animation's re-apply at a 3D load (src/sync/reapplyGate.ts): a
// guest's copy of a pillar takes the record's turn when its 3D comes; the
// host's own graph is its script's, except for a turn its script made while
// the pillar's 3D was away, which the engine never drew there.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { reapplyNow } from "../src/sync/reapplyGate";

test("a guest's kept turn plays when the pillar's 3D loads", () => {
  assert.strictEqual(reapplyNow({ hostedHere: false, viewPending: false, answered: false }), true);
});

test("a record still being applied waits: its own play shows the view", () => {
  assert.strictEqual(reapplyNow({ hostedHere: false, viewPending: true, answered: false }), false);
  assert.strictEqual(reapplyNow({ hostedHere: true, viewPending: true, answered: true }), false);
});

test("the host's own graph is its script's: a relayed turn is never replayed there", () => {
  assert.strictEqual(reapplyNow({ hostedHere: true, viewPending: false, answered: false }), false);
});

test("a turn the host's script made while the pillar's 3D was away shows when the host arrives", () => {
  assert.strictEqual(reapplyNow({ hostedHere: true, viewPending: false, answered: true }), true);
});
