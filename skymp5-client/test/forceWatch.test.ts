// A figure this seat's push knocked is claimed (src/sync/forceWatch.ts).
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { forceWatchVerdict } from "../src/sync/forceWatch";

test("a figure this seat's spell reached and its engine now reads knocked is claimed", () => {
  assert.equal(forceWatchVerdict({ since: 1000, now: 1400, ragdoll: true, tracked: false }), "claim");
});

test("a figure still standing within the window is watched on", () => {
  assert.equal(forceWatchVerdict({ since: 1000, now: 2900, ragdoll: false, tracked: false }), "wait");
});

test("a figure never knocked within the window leaves the watch", () => {
  assert.equal(forceWatchVerdict({ since: 1000, now: 3001, ragdoll: false, tracked: false }), "drop");
});

test("a figure already a row (followed, gripped or owned) is not claimed again", () => {
  assert.equal(forceWatchVerdict({ since: 1000, now: 1100, ragdoll: true, tracked: true }), "drop");
});
