// The guest's own game's watch of a look the host's script asks of it (src/sync/lookWatch.ts), JS state alone: the
// guest walks up to Bleak Falls' word wall, the host's run of the wall's script asks whether the guest sees the
// wall's look marker, and the guest's game answers from its own camera.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { LOOK_WATCH_MS, LookWatches } from "../src/sync/lookWatch";

const MARKER = 0x709d4;

test("an ask starts a watch whose first read is sent, looking away or not", () => {
  const w = new LookWatches();
  w.ask(MARKER, 0);
  assert.deepEqual(w.targets(0), [MARKER]);
  assert.strictEqual(w.read(MARKER, false), true);
});

test("the guest turns to the wall: the change is sent once, an unchanged read is not", () => {
  const w = new LookWatches();
  w.ask(MARKER, 0);
  w.read(MARKER, false);
  assert.strictEqual(w.read(MARKER, false), false);
  assert.strictEqual(w.read(MARKER, true), true);
  assert.strictEqual(w.read(MARKER, true), false);
});

test("each ask extends the watch and makes the next read due, so a lost answer is heard again", () => {
  const w = new LookWatches();
  w.ask(MARKER, 0);
  w.read(MARKER, true);
  w.ask(MARKER, 5000);
  assert.strictEqual(w.read(MARKER, true), true);
  assert.deepEqual(w.targets(5000 + LOOK_WATCH_MS - 1), [MARKER]);
});

test("a watch no ask renews ends, and a target no watch holds is never sent", () => {
  const w = new LookWatches();
  w.ask(MARKER, 0);
  assert.deepEqual(w.targets(LOOK_WATCH_MS), []);
  assert.strictEqual(w.read(MARKER, true), false);
  assert.strictEqual(w.read(0x12345, true), false);
});
