// A served graph state on a seat that is not its object's owner (src/sync/graphRestore.ts): restored in place of
// the record's view, unless a relay this seat took of the same object stands for it. The relay of the state's own
// seq is the same act: once it has played on this 3D a restore starts the act again, and when it waits for the
// copy's graph the restore stands for it, so the held play goes.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { graphRestore } from "../src/sync/graphRestore";

test("a relay past the state's seq stands: the state is not restored", () => {
  assert.deepStrictEqual(graphRestore({ last: 3, seq: 2, shownHere: true }), { why: "past", restore: false, dropHeld: false });
});

test("the relay of the state's own seq played on this 3D: the state is not restored over it", () => {
  assert.deepStrictEqual(graphRestore({ last: 1, seq: 1, shownHere: true }), { why: "played", restore: false, dropHeld: false });
});

test("the relay of the state's own seq still held for the copy's graph: the state is restored and the held play goes", () => {
  assert.deepStrictEqual(graphRestore({ last: 1, seq: 1, shownHere: false }), { why: "restore", restore: true, dropHeld: true });
});

test("no relay here, or an older one: the state is restored, and nothing held outlives it", () => {
  assert.deepStrictEqual(graphRestore({ last: undefined, seq: 2, shownHere: false }), { why: "restore", restore: true, dropHeld: true });
  assert.deepStrictEqual(graphRestore({ last: 1, seq: 2, shownHere: true }), { why: "restore", restore: true, dropHeld: true });
});
