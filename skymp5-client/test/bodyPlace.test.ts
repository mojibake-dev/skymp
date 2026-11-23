// Whether a reference's movement record places it (src/sync/bodyPlace.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { recordMovesIt } from "../src/sync/bodyPlace";

const none = { held: false, streaming: false, followedLive: false };

test("another player's figure this seat's grip holds takes no movement from its record", () => {
  assert.strictEqual(recordMovesIt({ isActor: true, tracked: true, ...none, held: true }), false);
});

test("a figure whose body this seat streams after the grab's release takes no movement from its record", () => {
  assert.strictEqual(recordMovesIt({ isActor: true, tracked: true, ...none, streaming: true }), false);
});

test("a figure following another seat's live stream of its body takes no movement from its record", () => {
  assert.strictEqual(recordMovesIt({ isActor: true, tracked: true, ...none, followedLive: true }), false);
});

test("a standing figure merely tracked in place keeps taking its record", () => {
  assert.strictEqual(recordMovesIt({ isActor: true, tracked: true, ...none }), true);
  assert.strictEqual(recordMovesIt({ isActor: true, tracked: false, ...none }), true);
});

test("a prop is placed by its record only while untracked", () => {
  assert.strictEqual(recordMovesIt({ isActor: false, tracked: true, ...none }), false);
  assert.strictEqual(recordMovesIt({ isActor: false, tracked: false, ...none }), true);
});
