// A container record burst kept as its newest (src/sync/latestPerKey.ts), JS
// state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { LatestPerKey } from "../src/sync/latestPerKey";

test("a burst of one key's values schedules one take, which gets the newest", () => {
  const q = new LatestPerKey<{ arrows: number }>();
  assert.strictEqual(q.put(0x267e8, { arrows: 60 }), true);
  assert.strictEqual(q.put(0x267e8, { arrows: 174 }), false);
  assert.strictEqual(q.put(0x267e8, { arrows: 175 }), false);
  assert.deepStrictEqual(q.take(0x267e8), { arrows: 175 });
  assert.strictEqual(q.take(0x267e8), undefined);
  // the next burst schedules again
  assert.strictEqual(q.put(0x267e8, { arrows: 1 }), true);
});

test("keys are kept apart", () => {
  const q = new LatestPerKey<string>();
  assert.strictEqual(q.put(0x267e8, "chest"), true);
  assert.strictEqual(q.put(0x9dc64, "evidence"), true);
  assert.strictEqual(q.take(0x9dc64), "evidence");
  assert.strictEqual(q.take(0x267e8), "chest");
});

// An apply lays (record - the container's count as read), and its changes land
// only after the tick, so every apply in one tick reads the same count
const burst = (held: number, records: number[]): number => held + records.reduce((added, record) => added + (record - held), 0);

test("N applies of one record in a tick lay its difference N times; the newest alone lands the record", () => {
  const held = 60;
  const records: number[] = new Array(12).fill(174);
  assert.strictEqual(burst(held, records), 60 + 12 * 114);
  const q = new LatestPerKey<number>();
  let scheduled = 0;
  for (const record of records) {
    if (q.put(0x267e8, record)) {
      scheduled += 1;
    }
  }
  assert.strictEqual(scheduled, 1);
  const newest = q.take(0x267e8);
  assert.notStrictEqual(newest, undefined);
  assert.strictEqual(burst(held, [newest as number]), 174);
});
