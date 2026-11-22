// A served corpse record's pass (src/sync/corpseLay.ts), JS state alone: compiled by the
// package's own tsc into build/test and run with plain node (yarn test).
"use strict";
const assert = require("assert");
const { corpsePass, outlivesHold, HOLD_MS } = require("../build/test/corpseLay.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

// a record on a dead copy another seat simulates, nothing in its way
const facts = (over) => Object.assign({
  applyOff: false,
  queued: false,
  reconcileOwed: false,
  menuOpen: false,
  actor: true,
  dead: true,
  bind: false,
  simulatesHere: false,
  followed: false,
}, over);

test("a record on a dead copy another seat simulates is laid", () => {
  assert.strictEqual(corpsePass(facts({})), "lay");
});

test("a record is never laid while this seat's ContainerMenu is open", () => {
  assert.strictEqual(corpsePass(facts({ menuOpen: true })), "menu");
});

test("a record the menu held past the hold is neither laid nor dropped, and is laid once the menu closes", () => {
  const heldMs = HOLD_MS + 10000;
  assert.strictEqual(corpsePass(facts({ menuOpen: true })), "menu");
  assert.strictEqual(outlivesHold(heldMs, true), false);
  assert.strictEqual(corpsePass(facts({ menuOpen: false })), "lay");
});

test("a record held past the hold for a kill that never came is dropped", () => {
  assert.strictEqual(corpsePass(facts({ dead: false })), "kill");
  assert.strictEqual(outlivesHold(HOLD_MS, false), true);
  assert.strictEqual(outlivesHold(HOLD_MS - 1, false), false);
});

test("a laid body's inventory reconcile waits while this seat's ContainerMenu is open", () => {
  assert.strictEqual(corpsePass(facts({ reconcileOwed: true, menuOpen: true })), "menu");
});

test("a laid body's inventory is reconciled once the menu is closed", () => {
  assert.strictEqual(corpsePass(facts({ reconcileOwed: true })), "reconcile");
});

test("a copy alive here holds its record for the kill", () => {
  assert.strictEqual(corpsePass(facts({ dead: false })), "kill");
});

test("a seat that simulates the body keeps its own engine's death, unless the pass is the bind's", () => {
  assert.strictEqual(corpsePass(facts({ simulatesHere: true })), "simulated");
  assert.strictEqual(corpsePass(facts({ simulatesHere: true, bind: true })), "lay");
});

test("a followed body is laid only once its follow rests", () => {
  assert.strictEqual(corpsePass(facts({ followed: true })), "follow");
});

test("an apply in flight and the lever hold the record", () => {
  assert.strictEqual(corpsePass(facts({ queued: true })), "queued");
  assert.strictEqual(corpsePass(facts({ applyOff: true })), "off");
});

test("no actor form here holds the record", () => {
  assert.strictEqual(corpsePass(facts({ actor: false, dead: false })), "no-actor");
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
