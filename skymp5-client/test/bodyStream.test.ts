// A dead body's path from the seat that simulates it (src/sync/bodyStream.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { DeadBodyFacts, streamCarries, trackedInPlace } from "../src/sync/bodyStream";

const body = (over: Partial<DeadBodyFacts>): DeadBodyFacts => Object.assign({ held: false, streaming: false, named: false }, over);

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
