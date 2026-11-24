// A guest's dialogue fragment unlocking a gate the host's game owns: the platform skips the call on the guest and
// batches it as kind "forward" (src/sync/scriptForward.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { forwardMark, isSent, keepsForward } from "../src/sync/scriptForward";

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

test("a guest's gate-stop stage's unlock, a quest whose stages this seat does not relay, is forwarded", () => {
  const relays = (quest: number) => quest !== 0xd1984;
  assert.strictEqual(keepsForward(0xd1984, relays), true);
});

test("a forward from a relayed quest's stage is left to the owner's own run; one from any other root is sent", () => {
  const relays = () => true;
  assert.strictEqual(keepsForward(0x74a83, relays), false);
  assert.strictEqual(keepsForward(undefined, relays), true);
  assert.strictEqual(keepsForward(0, relays), true);
});
