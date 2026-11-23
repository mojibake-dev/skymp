// What a no-row verdict writes into the kind cache (src/sync/kindCache.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { noRowEntry } from "../src/sync/kindCache";

test("a thrown player's figure dropped as no row is gripped again: its kind is left to its next read", () => {
  // a living actor's body is judged again at every read and no rescan takes one, so the verdict caches nothing
  assert.strictEqual(noRowEntry(true, 1000), undefined);
});

test("a dead body or a prop dropped as no row keeps the verdict until it is judged again", () => {
  assert.deepStrictEqual(noRowEntry(false, 1000), { world: null, noRowAt: 1000 });
});
