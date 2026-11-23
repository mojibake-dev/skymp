// Whether a figure's record glide is stopped now (src/sync/translationStop.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { translationStopDue } from "../src/sync/translationStop";

test("a figure whose body goes down here has its glide stopped once", () => {
  assert.strictEqual(translationStopDue(false, true), true);
});

test("a figure that stays down is not stopped again", () => {
  assert.strictEqual(translationStopDue(true, true), false);
});

test("a standing figure keeps its glide", () => {
  assert.strictEqual(translationStopDue(false, false), false);
  assert.strictEqual(translationStopDue(true, false), false);
});

test("a figure down again after its get-up is stopped again", () => {
  const steps: Array<[boolean, boolean]> = [[false, true], [true, false], [false, true]];
  assert.deepStrictEqual(steps.map(([was, now]) => translationStopDue(was, now)), [true, false, true]);
});
