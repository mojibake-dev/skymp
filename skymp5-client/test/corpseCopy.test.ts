// Whether this seat's copy of a body can take its change-form record now (src/sync/corpseCopy.ts), JS state alone:
// the watcher of a raised thrall walks out of range and back, and its game spends no try on a copy with no 3D.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { corpseCopyWait } from "../src/sync/corpseCopy";

test("a body whose copy here has no 3D waits for its 3D and spends no try", () => {
  assert.strictEqual(corpseCopyWait({ dead: true, loaded3D: false }), "3d");
});

test("a dead copy with its 3D takes the record now", () => {
  assert.strictEqual(corpseCopyWait({ dead: true, loaded3D: true }), undefined);
});

test("a copy alive here waits for the kill, whether or not its 3D is loaded", () => {
  assert.strictEqual(corpseCopyWait({ dead: false, loaded3D: true }), "kill");
  assert.strictEqual(corpseCopyWait({ dead: false, loaded3D: false }), "kill");
});
