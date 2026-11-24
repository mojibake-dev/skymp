// Whether the player's own body stays tracked under another seat's word (src/sync/ownBodyKept.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { ownBodyKeptForTheirWord } from "../src/sync/ownBodyKept";

const ME = 0xff000001;
const GRIPPER = 0xff000000;

test("the player's own body another seat's grip owns stays tracked, however still the grip holds it", () => {
  assert.strictEqual(ownBodyKeptForTheirWord({ ownPlayer: true, wordPlayer: GRIPPER, myId: ME }), true);
});

test("the player's own body the cell's host or this seat owns is kept by the usual rules alone", () => {
  assert.strictEqual(ownBodyKeptForTheirWord({ ownPlayer: true, wordPlayer: 0, myId: ME }), false);
  assert.strictEqual(ownBodyKeptForTheirWord({ ownPlayer: true, wordPlayer: ME, myId: ME }), false);
});

test("another reference another seat owns is kept by its stream alone", () => {
  assert.strictEqual(ownBodyKeptForTheirWord({ ownPlayer: false, wordPlayer: GRIPPER, myId: ME }), false);
});

test("before this seat knows its own id nothing is kept for a word", () => {
  assert.strictEqual(ownBodyKeptForTheirWord({ ownPlayer: true, wordPlayer: GRIPPER, myId: 0 }), false);
});
