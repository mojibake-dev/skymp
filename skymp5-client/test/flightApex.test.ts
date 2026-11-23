// A followed body's flight apex (src/sync/flightApex.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { FlightApex } from "../src/sync/flightApex";

const flight = (samples: Array<[number, boolean]>) => {
  const a = new FlightApex();
  for (const [z, held] of samples) {
    a.note(z, held);
  }
  return a.apex();
};

test("a thrown body's apex is its highest root after the grip lets go", () => {
  assert.strictEqual(flight([[100, true], [120, true], [130, false], [300, false], [50, false]]), 300);
});

test("a held body's height is no fall: a body lowered and dropped falls from where it was let go", () => {
  assert.strictEqual(flight([[500, true], [200, true], [200, false], [0, false]]), 200);
});

test("a new grip starts the apex over", () => {
  assert.strictEqual(flight([[100, true], [300, false], [0, false], [100, true], [120, false], [0, false]]), 120);
});

test("a stream with no grab (a shout's throw) counts from its first sample", () => {
  assert.strictEqual(flight([[40, false], [260, false], [10, false]]), 260);
});

test("a body still held has flown nothing", () => {
  assert.strictEqual(flight([[100, true], [400, true]]), undefined);
});

test("a root that is not a number counts for nothing", () => {
  assert.strictEqual(flight([[NaN, false], [80, false], [Infinity, false]]), 80);
});
