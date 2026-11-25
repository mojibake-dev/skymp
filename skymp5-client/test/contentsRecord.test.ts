// A dead body's contents changed on its owner's engine (src/sync/contentsRecord.ts): recorded at once when the body
// lies still, so a take shows on every seat; a body whose fall this seat still runs waits for its rest's record,
// since a record taken mid-fall holds the fall's poses and its dying life state, and a seat that lays its copy by it
// puts the body back where it died to fall again.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { recordContentsNow } from "../src/sync/contentsRecord";

test("a dead body lying still is recorded at once when its contents change", () => {
  assert.strictEqual(recordContentsNow({ dead: true, loaded3D: true, falling: false }), true);
});

test("a body whose fall this seat still runs waits for its rest's record", () => {
  assert.strictEqual(recordContentsNow({ dead: true, loaded3D: true, falling: true }), false);
});

test("a living actor, or a body with no 3D here, is not recorded", () => {
  assert.strictEqual(recordContentsNow({ dead: false, loaded3D: true, falling: false }), false);
  assert.strictEqual(recordContentsNow({ dead: true, loaded3D: false, falling: false }), false);
});
