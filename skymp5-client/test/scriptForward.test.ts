// A guest's dialogue fragment unlocking a gate the host's game owns: the platform skips the call on the guest and
// batches it as kind "forward" (src/sync/scriptForward.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { forwardMark, isSent } from "../src/sync/scriptForward";

test("a forward is sent as a script call, marked forward", () => {
  assert.strictEqual(isSent("forward"), true);
  assert.deepEqual(forwardMark("forward"), { forward: true });
});

test("an action and a creation are sent unmarked", () => {
  for (const kind of ["action", "created"] as const) {
    assert.strictEqual(isSent(kind), true, kind);
    assert.deepEqual(forwardMark(kind), {}, kind);
  }
});

test("a state row's call, a touch, an everyone row's call and a look are never sent as a forward", () => {
  for (const kind of ["state", "touch", "everyone", "look"] as const) {
    assert.strictEqual(isSent(kind), false, kind);
    assert.deepEqual(forwardMark(kind), {}, kind);
  }
});
