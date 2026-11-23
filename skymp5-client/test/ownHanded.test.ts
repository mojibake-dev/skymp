// This game's own player's body under another player's force sends no
// movement of its own (src/sync/knockState.ts, the own-handed mark).
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { clearOwnHanded, isOwnHanded, markOwnHanded } from "../src/sync/knockState";

test("the own body handed to another player's grip or throw holds this game's movement sends", () => {
  clearOwnHanded();
  markOwnHanded(1000);
  assert.equal(isOwnHanded(1500), true);
});

test("the hand-back word clears it, and movement is sent again", () => {
  markOwnHanded(1000);
  clearOwnHanded();
  assert.equal(isOwnHanded(1100), false);
});

test("a mark left unrenewed lapses, so a lost clear holds movement for two seconds at most", () => {
  clearOwnHanded();
  markOwnHanded(1000);
  assert.equal(isOwnHanded(3000), true);
  assert.equal(isOwnHanded(3001), false);
});

test("nothing marked, nothing held", () => {
  clearOwnHanded();
  assert.equal(isOwnHanded(1000), false);
});
