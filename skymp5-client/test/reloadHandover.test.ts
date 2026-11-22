// A hot reload's hand-over (src/sync/reloadHandover.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { lostAtReload, teardownLoses } from "../src/sync/reloadHandover";

test("a reload's teardown of the kept view loses no form, so it demotes nothing", () => {
  assert.equal(teardownLoses("reload"), false);
});

test("a form the model let go, and a worldspace change, are losses as before", () => {
  assert.equal(teardownLoses("model"), true);
  assert.equal(teardownLoses("world"), true);
});

test("the hosted actors the outgoing client's teardown took from the platform's set are set again", () => {
  const platform = new Set([0x373a6]);
  assert.deepEqual(lostAtReload([0x373a6, 0x375aa, 0x3b9e6], (id) => platform.has(id)), [0x375aa, 0x3b9e6]);
});

test("nothing is set again when the platform's set kept them all, or the restored set holds none", () => {
  assert.deepEqual(lostAtReload([0x373a6], () => true), []);
  assert.deepEqual(lostAtReload([], () => false), []);
});
