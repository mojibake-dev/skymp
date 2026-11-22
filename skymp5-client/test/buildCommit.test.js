// The commit a client bundle names (src/sync/buildCommit.ts), JS state alone: compiled by the
// package's own tsc into build/test and run with plain node (yarn test).
"use strict";
const assert = require("assert");
const { BUILD_MARKER, commitOf, buildLine } = require("../build/test/buildCommit.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);
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

let failed = 0;
for (const [name, fn] of cases) {
  try {
    fn();
    console.log(`ok - ${name}`);
  } catch (e) {
    failed += 1;
    console.log(`not ok - ${name}\n${e && e.stack ? e.stack : e}`);
  }
}
console.log(`${cases.length - failed} of ${cases.length} passed`);
process.exit(failed === 0 ? 0 : 1);
