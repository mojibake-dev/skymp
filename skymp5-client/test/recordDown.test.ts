// Whether an actor's copy is down for its movement record (src/sync/recordDown.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { recordDown } from "../src/sync/recordDown";

const up = { relayedRagdoll: false, knockedDown: false, figure: true, ragdollHere: false };

test("a figure this copy's engine holds ragdolled after a grab's throw is not moved by its record", () => {
  assert.strictEqual(recordDown({ ...up, ragdollHere: true }), true);
});

test("a standing figure is still moved by its record", () => {
  assert.strictEqual(recordDown(up), false);
});

test("a relayed ragdoll or a knock this game marked keeps the copy down, as before", () => {
  assert.strictEqual(recordDown({ ...up, relayedRagdoll: true }), true);
  assert.strictEqual(recordDown({ ...up, knockedDown: true }), true);
});

test("an NPC copy's engine ragdoll is read through its knock alone", () => {
  assert.strictEqual(recordDown({ ...up, figure: false, ragdollHere: true }), false);
});
