// Which parts of a live body the engine drives (src/sync/drivenParts.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { learnedUndriven, pastReachStep, placeUndriven, skeletonKey, undrivenIn, UNDRIVEN_WAIT_MS } from "../src/sync/drivenParts";

// a beast's ragdoll, knocked: the COM and legs dynamic, the tail keyframed
const knocked = [
  { node: "NPC COM [COM ]", motion: 2 },
  { node: "NPC L Thigh [LThg]", motion: 2 },
  { node: "NPC L Calf [LClf]", motion: 3 },
  { node: "TailBone01", motion: 4 },
  { node: "TailBone02", motion: 4 },
  { node: "NPC Spine [Spn0]", motion: 2 },
];

test("a ragdolled read's parts that stay keyframed while the rest are dynamic are undriven", () => {
  assert.deepStrictEqual(undrivenIn(knocked, true), [3, 4]);
});

test("a read where no part is dynamic, a hold's or a standing body's, says nothing", () => {
  assert.strictEqual(undrivenIn(knocked.map((p) => ({ ...p, motion: 4 })), true), undefined);
});

test("a read of an actor not ragdolled now says nothing", () => {
  assert.strictEqual(undrivenIn(knocked, false), undefined);
});

test("an undriven part is sent at part 0's place and the rest as read", () => {
  const read = [[0, 0, 0], [10, 0, 0], [20, 0, 0], [700, 0, 0], [710, 0, 0], [0, 0, 30]];
  assert.deepStrictEqual(placeUndriven(read, new Set([3, 4])), [[0, 0, 0], [10, 0, 0], [20, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 30]]);
});

test("with no undriven part known every part is sent as read", () => {
  const read = [[0, 0, 0], [700, 0, 0]];
  assert.deepStrictEqual(placeUndriven(read, new Set()), read);
});

test("a skeleton is its parts' names in walk order", () => {
  assert.strictEqual(skeletonKey(knocked.slice(0, 2)), "NPC COM [COM ]|NPC L Thigh [LThg]");
});

test("a grip's first sample past reach, its skeleton not yet learned, waits for the ragdoll's read", () => {
  assert.strictEqual(pastReachStep(false, 0), "wait");
  assert.strictEqual(pastReachStep(false, UNDRIVEN_WAIT_MS - 1), "wait");
});

test("past the wait, or with the skeleton learned, a part still past reach drops the row", () => {
  assert.strictEqual(pastReachStep(false, UNDRIVEN_WAIT_MS), "drop");
  assert.strictEqual(pastReachStep(true, 0), "drop");
});

test("a later read with every part dynamic keeps the skeleton's undriven parts", () => {
  assert.deepStrictEqual([...learnedUndriven(new Set([7, 8, 9, 10, 11]), [])].sort((a, b) => a - b), [7, 8, 9, 10, 11]);
});

test("a read that finds another undriven part adds it", () => {
  assert.deepStrictEqual([...learnedUndriven(new Set([7, 8]), [8, 9])].sort((a, b) => a - b), [7, 8, 9]);
  assert.deepStrictEqual([...learnedUndriven(undefined, [3])], [3]);
});
