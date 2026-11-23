// The commit a client bundle names (src/sync/buildCommit.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { BUILD_MARKER, buildLine, commitOf } from "../src/sync/buildCommit";

const SHA = "8a340f676fde0123456789abcdef0123456789ab";

test("a bundle built at a commit names it", () => {
  assert.deepStrictEqual(commitOf(BUILD_MARKER + SHA), { sha: SHA, dirty: false });
  assert.strictEqual(buildLine(BUILD_MARKER + SHA), `skymp5-client ${SHA}`);
});

test("a bundle built from a tree that differs from its commit says so", () => {
  assert.deepStrictEqual(commitOf(BUILD_MARKER + SHA + "-dirty"), { sha: SHA, dirty: true });
  assert.strictEqual(buildLine(BUILD_MARKER + SHA + "-dirty"), `skymp5-client ${SHA}, built from a tree that differs from it`);
});

test("a build with no git names no commit", () => {
  assert.strictEqual(commitOf(BUILD_MARKER), undefined);
  assert.strictEqual(commitOf(undefined), undefined);
  assert.strictEqual(commitOf(BUILD_MARKER + "not-a-sha"), undefined);
  assert.strictEqual(buildLine(BUILD_MARKER), "skymp5-client of no recorded commit");
});
