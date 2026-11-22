// A dead body's path from the seat that simulates it (src/sync/bodyStream.ts), JS state alone:
// compiled by the package's own tsc into build/test and run with plain node (yarn test).
"use strict";
const assert = require("assert");
const { streamCarries, trackedInPlace } = require("../build/test/bodyStream.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

const body = (over) => Object.assign({ held: false, streaming: false, named: false }, over);

test("a fresh kill's body whose death word names this seat falls on the prop stream from the word", () => {
  assert.strictEqual(streamCarries(body({ named: true })), true);
});

test("a body no word names this seat for stays on the death settle", () => {
  assert.strictEqual(streamCarries(body({})), false);
});

test("a hand on the body or its open stream carries it", () => {
  assert.strictEqual(streamCarries(body({ held: true })), true);
  assert.strictEqual(streamCarries(body({ streaming: true })), true);
});

test("a dead body first tracked under this seat's death word is tracked in place, no grace", () => {
  assert.strictEqual(trackedInPlace(false, true, true), true);
});

test("a dead body another seat simulates waits the grace", () => {
  assert.strictEqual(trackedInPlace(false, true, false), false);
});

test("a knocked live body is tracked in place", () => {
  assert.strictEqual(trackedInPlace(true, false, false), true);
});

test("an item a word names this seat for waits the grace: its copy may still be spawning", () => {
  assert.strictEqual(trackedInPlace(false, false, true), false);
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
