// Which record moves the tail probe logs (src/sync/tailProbeRule.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { tailProbeLogs, TAIL_FIRST_MOVES } from "../src/sync/tailProbeRule";

const steady = { kind: "translateTo" as const, before: 12, after: 14, moves: TAIL_FIRST_MOVES };

test("every teleport of a figure is logged", () => {
  assert.strictEqual(tailProbeLogs({ ...steady, kind: "setPosition" }), true);
});

test("the first moves after the figure is made are logged", () => {
  assert.strictEqual(tailProbeLogs({ ...steady, moves: 0 }), true);
  assert.strictEqual(tailProbeLogs({ ...steady, moves: TAIL_FIRST_MOVES - 1 }), true);
});

test("a glide that opens the tail, or leaves it far, is logged", () => {
  assert.strictEqual(tailProbeLogs({ ...steady, before: 10, after: 80 }), true);
  assert.strictEqual(tailProbeLogs({ ...steady, before: 300, after: 310 }), true);
});

test("the steady walk is not logged", () => {
  assert.strictEqual(tailProbeLogs(steady), false);
});
