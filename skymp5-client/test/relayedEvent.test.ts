// A relayed press on the owner's game (src/sync/relayedEvent.ts), JS state alone: one whose target or presser this
// game holds no copy of is said in a line, never dropped in silence.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { relayDropLine } from "../src/sync/relayedEvent";

const LEVER = 0xaa8c3;
const GUEST = 0xff000001;

test("a relayed press whose presser has no figure here is said", () => {
  assert.strictEqual(relayDropLine(LEVER, GUEST, true, false), "relayed activation aa8c3 by ff000001 not played: no presser here");
});

test("a relayed press whose target is not here is said", () => {
  assert.strictEqual(relayDropLine(LEVER, GUEST, false, true), "relayed activation aa8c3 by ff000001 not played: no target here");
  assert.strictEqual(relayDropLine(LEVER, GUEST, false, false), "relayed activation aa8c3 by ff000001 not played: no target or presser here");
});
