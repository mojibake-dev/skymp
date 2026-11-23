// Whether a body is tracked where its engine has it (src/sync/gripRow.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { placeRefusal } from "../src/sync/gripRow";

// Whiterun's worldspace, where the player stands
const whiterun = 0x1a26f;
const body = { loaded3D: true, playerWorldOrCell: whiterun, refWorldOrCell: whiterun };

test("a second grip of the other player's figure tracks it, whatever world or cell the thrown figure reads", () => {
  assert.strictEqual(placeRefusal({ ...body, gripped: true }), undefined);
  assert.strictEqual(placeRefusal({ ...body, refWorldOrCell: 0, gripped: true }), undefined);
  assert.strictEqual(placeRefusal({ ...body, refWorldOrCell: 0x3c, gripped: true }), undefined);
});

test("a grip still needs the body's 3D", () => {
  assert.strictEqual(placeRefusal({ ...body, loaded3D: false, gripped: true }), "not loaded");
});

test("a body nobody grips keeps the world-or-cell check", () => {
  assert.strictEqual(placeRefusal({ ...body, gripped: false }), undefined);
  assert.strictEqual(placeRefusal({ ...body, refWorldOrCell: 0, gripped: false }), "another world or cell");
  assert.strictEqual(placeRefusal({ ...body, refWorldOrCell: 0x3c, gripped: false }), "another world or cell");
  assert.strictEqual(placeRefusal({ ...body, loaded3D: false, gripped: false }), "not loaded");
  // a player whose own place reads none takes any body's
  assert.strictEqual(placeRefusal({ ...body, playerWorldOrCell: 0, refWorldOrCell: 0x3c, gripped: false }), undefined);
});
