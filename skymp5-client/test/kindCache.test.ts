// What a no-row verdict writes into the kind cache, and how long it holds (src/sync/kindCache.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { liveNoRowHeld, noRowEntry } from "../src/sync/kindCache";

// propService's LIVE_NO_ROW_MS
const HOLD = 2_000;

test("a torn player's figure the grab names every frame is held as no row for a window, then judged again", () => {
  // the live-body marks, so the kind's live read sees the verdict instead of judging the figure anew each frame
  assert.deepStrictEqual(noRowEntry(true, 1000), { world: null, noRowAt: 1000, body: true, live: true });
  assert.strictEqual(liveNoRowHeld(1000, 1015, HOLD), true);
  assert.strictEqual(liveNoRowHeld(1000, 1000 + HOLD - 1, HOLD), true);
  // after the window the figure is judged again, so a grip of it once whole holds
  assert.strictEqual(liveNoRowHeld(1000, 1000 + HOLD, HOLD), false);
  assert.strictEqual(liveNoRowHeld(undefined, 1000, HOLD), false);
});

test("a dead body or a prop dropped as no row keeps the verdict until it is judged again", () => {
  assert.deepStrictEqual(noRowEntry(false, 1000), { world: null, noRowAt: 1000 });
});
