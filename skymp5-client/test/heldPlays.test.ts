// A relayed animation the watcher's copy refused (src/sync/heldPlays.ts), JS state alone. The owner's web plays
// "Exit" just after it is destroyed; the watcher's copy took the destroyed record first, and its new model's graph
// refused the play. The play is held and tried again on each update until the graph takes it, at most HELD_PLAY_MS.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { HELD_PLAY_MS, HeldPlays } from "../src/sync/heldPlays";

const WEB = 0x3c2e1;
const REMOTE = 0x3c2e1;

test("a refused play is held and tried again until its copy's graph takes it", () => {
  const held = new HeldPlays();
  held.hold(WEB, REMOTE, "Exit", 0);
  assert.deepEqual(held.due(16).retry.map((p) => [p.id, p.event]), [[WEB, "Exit"]]);
  assert.deepEqual(held.due(33).retry.map((p) => p.event), ["Exit"]);
  const taken = held.taken(WEB, 440);
  assert.deepEqual(taken && [taken.event, taken.heldMs], ["Exit", 440]);
  assert.deepEqual(held.due(50), { retry: [], expired: [] });
});

test("a play its graph never takes is dropped at the bound, once", () => {
  const held = new HeldPlays();
  held.hold(WEB, REMOTE, "Exit", 0);
  assert.strictEqual(held.due(HELD_PLAY_MS - 1).retry.length, 1);
  const end = held.due(HELD_PLAY_MS);
  assert.deepEqual([end.retry.length, end.expired.map((p) => p.event)], [0, ["Exit"]]);
  assert.deepEqual(held.due(HELD_PLAY_MS + 1), { retry: [], expired: [] });
});

test("a later relay of the same reference supersedes a held play", () => {
  const held = new HeldPlays();
  held.hold(WEB, REMOTE, "Exit", 0);
  held.drop(WEB);
  assert.deepEqual(held.due(16), { retry: [], expired: [] });
  held.hold(WEB, REMOTE, "Exit", 0);
  held.hold(WEB, REMOTE, "Close", 100);
  assert.deepEqual(held.due(200).retry.map((p) => [p.event, p.since]), [["Close", 100]]);
});

test("a take of a reference nothing holds answers nothing", () => {
  assert.strictEqual(new HeldPlays().taken(WEB, 0), undefined);
});
