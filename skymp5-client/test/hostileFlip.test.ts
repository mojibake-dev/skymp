// Whether a hosted actor's relayed combat target turned to this seat's player (src/sync/hostileFlip.ts), JS state alone:
// the turn that puts a bandit the host runs on the guest's compass and the one that takes it off.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { hostileFlip } from "../src/sync/hostileFlip";

const guest = 0xff000001;
const host = 0xff000000;

test("a bandit the host runs that turns on the guest goes on the guest's compass", () => {
  assert.strictEqual(hostileFlip(0, guest, guest), true);
  assert.strictEqual(hostileFlip(host, guest, guest), true);
});

test("a bandit that turns away from the guest, or stops fighting, comes off it", () => {
  assert.strictEqual(hostileFlip(guest, host, guest), false);
  assert.strictEqual(hostileFlip(guest, 0, guest), false);
});

test("a bandit's turn between others leaves the guest's compass as it was", () => {
  assert.strictEqual(hostileFlip(0, host, guest), undefined);
  assert.strictEqual(hostileFlip(guest, guest, guest), undefined);
  assert.strictEqual(hostileFlip(0, guest, 0), undefined);
});
