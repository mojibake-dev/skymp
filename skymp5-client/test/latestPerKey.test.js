// A container record burst kept as its newest (src/sync/latestPerKey.ts), JS
// state alone: compiled by the package's own tsc into build/test and run with
// plain node (yarn test).
"use strict";
const assert = require("assert");
const { LatestPerKey } = require("../build/test/latestPerKey.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

test("a burst of one key's values schedules one take, which gets the newest", () => {
  const q = new LatestPerKey();
  assert.strictEqual(q.put(0x267e8, { arrows: 60 }), true);
  assert.strictEqual(q.put(0x267e8, { arrows: 174 }), false);
  assert.strictEqual(q.put(0x267e8, { arrows: 175 }), false);
  assert.deepStrictEqual(q.take(0x267e8), { arrows: 175 });
  assert.strictEqual(q.take(0x267e8), undefined);
  // the next burst schedules again
  assert.strictEqual(q.put(0x267e8, { arrows: 1 }), true);
});

test("keys are kept apart", () => {
  const q = new LatestPerKey();
  assert.strictEqual(q.put(0x267e8, "chest"), true);
  assert.strictEqual(q.put(0x9dc64, "evidence"), true);
  assert.strictEqual(q.take(0x9dc64), "evidence");
  assert.strictEqual(q.take(0x267e8), "chest");
});

// An apply lays (record - the container's count as read), and its changes land
// only after the tick, so every apply in one tick reads the same count
const burst = (held, records) => held + records.reduce((added, record) => added + (record - held), 0);

test("N applies of one record in a tick lay its difference N times; the newest alone lands the record", () => {
  const held = 60;
  const records = new Array(12).fill(174);
  assert.strictEqual(burst(held, records), 60 + 12 * 114);
  const q = new LatestPerKey();
  let scheduled = 0;
  for (const record of records) {
    if (q.put(0x267e8, record)) {
      scheduled += 1;
    }
  }
  assert.strictEqual(scheduled, 1);
  assert.strictEqual(burst(held, [q.take(0x267e8)]), 174);
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
