// The place a body of parts' sample is anchored at (src/sync/boneAnchor.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { Point, boneAnchor, partsPast } from "../src/sync/boneAnchor";

const REACH = 512;
const root: Point = [0, 0, 0];

// a thrown figure's ragdoll, flown from a root left standing: its COM (part
// 0) 500 units off, part 7 517 units off, every part within 40 of the COM
const flown: Point[] = [[500, 0, 0], [505, 20, 0], [495, -20, 0], [510, 10, 30], [490, -10, 30], [505, 5, -30], [498, 0, 40], [517, 0, 0]];

test("a long throw's ragdoll, a part 517 units from its root, anchors at its first part and keeps its row", () => {
  const a = boneAnchor(root, flown, REACH, false);
  assert.deepStrictEqual(a, { pos: [500, 0, 0], atPart: true });
});

test("a stream anchored at its first part stays there, though its parts come back within reach of the root", () => {
  const back: Point[] = flown.map((p) => [p[0] - 200, p[1], p[2]]);
  assert.deepStrictEqual(boneAnchor(root, back, REACH, true), { pos: [300, 0, 0], atPart: true });
});

test("a body within reach of its root keeps the root's place", () => {
  const near: Point[] = [[10, 0, 0], [20, 5, 0], [0, 30, 10]];
  assert.deepStrictEqual(boneAnchor(root, near, REACH, false), { pos: root, atPart: false });
});

test("a body whose parts lie past reach of its first part too is no row, the part and its distance named", () => {
  const split: Point[] = [[10, 0, 0], [20, 0, 0], [600, 0, 0]];
  const a = boneAnchor(root, split, REACH, false);
  assert.ok("past" in a);
  assert.strictEqual(a.past, 2);
  assert.strictEqual(Math.round(a.reach), 590);
});

test("a torn body's line names every part past reach of its first part, with its distance from that part and from the root", () => {
  // the COM (part 0) held 560 units behind, the rest of the ragdoll flown on together near its root
  const torn: Point[] = [[0, 0, 0], [10, 0, 0], [20, 0, 0], [560, 0, 0], [570, 10, 0]];
  const root: Point = [565, 0, 0];
  const past = partsPast(root, torn, 512);
  assert.deepStrictEqual(past.map((p) => p.index), [3, 4]);
  assert.strictEqual(Math.round(past[0].fromFirst), 560);
  assert.strictEqual(Math.round(past[0].fromRoot), 5);
  assert.strictEqual(Math.round(past[1].fromFirst), 570);
  // a whole body has none
  assert.deepStrictEqual(partsPast([0, 0, 0], [[0, 0, 0], [30, 0, 0]], 512), []);
});
