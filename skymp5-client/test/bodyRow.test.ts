// Whether an actor's body is a prop row on this seat (src/sync/bodyRow.ts).
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { bodyHasNoRow } from "../src/sync/bodyRow";

const living = { dead: false, own: false, followed: false, pushedHere: false };

test("another player's figure this seat's own player's push threw is a row: its impulse claims the body", () => {
  assert.equal(bodyHasNoRow({ ...living, serverMade: true, pushedHere: true }), false);
});

test("another player's standing figure nobody here pushed, followed or gripped is no row", () => {
  assert.equal(bodyHasNoRow({ ...living, serverMade: true }), true);
});

test("another player's figure followed or gripped here is a row, as before", () => {
  assert.equal(bodyHasNoRow({ ...living, serverMade: true, followed: true }), false);
});

test("a game-file NPC and this seat's own player keep their rules", () => {
  assert.equal(bodyHasNoRow({ ...living, serverMade: false }), false);
  assert.equal(bodyHasNoRow({ ...living, serverMade: false, own: true }), false);
  assert.equal(bodyHasNoRow({ ...living, dead: true, serverMade: true }), true);
  assert.equal(bodyHasNoRow({ ...living, dead: true, serverMade: false, own: true }), true);
  assert.equal(bodyHasNoRow({ ...living, dead: true, serverMade: false }), false);
});
