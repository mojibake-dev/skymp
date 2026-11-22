// The view's dead check (src/sync/deadCheck.ts), JS state alone: compiled by
// the package's own tsc into build/test and run with plain node (yarn test).
"use strict";
const assert = require("assert");
const { deadCheckAsk } = require("../build/test/deadCheck.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

const base = { engineDead: false, want: false, simulatedHere: false, bind: false, ownDeathMsAgo: undefined, wordWaitMs: 3000 };

test("the host kills an NPC: it stays down on the host's screen while the server's word on the death is on its way", () => {
  // the host's engine killed it 73 ms ago and reported it; the model still holds the alive word from before the death
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: true, want: false, simulatedHere: true, ownDeathMsAgo: 73 }), "none");
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: true, want: false, simulatedHere: true, ownDeathMsAgo: 2999 }), "none");
});

test("a host's report the server never answered: the body stands up by the record's alive word after the wait", () => {
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: true, want: false, simulatedHere: true, ownDeathMsAgo: 3000 }), "standUp");
});

test("a live word with no own death waiting stands the body up, on the host and on a watcher", () => {
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: true, want: false, simulatedHere: true }), "standUp");
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: true, want: false, simulatedHere: false }), "standUp");
  // a watcher holds no report of its own: the host's death report is the host's
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: true, want: false, simulatedHere: false, ownDeathMsAgo: 73 }), "standUp");
});

test("the kill rules: a watcher kills by the dead word, the host only at the bind, and nothing when the engine agrees", () => {
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: false, want: true, simulatedHere: false }), "kill");
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: false, want: true, simulatedHere: true }), "none");
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: false, want: true, simulatedHere: true, bind: true }), "bindKill");
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: false, want: true, simulatedHere: false, bind: true }), "bindKill");
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: true, want: true, simulatedHere: true, ownDeathMsAgo: 73 }), "none");
  assert.strictEqual(deadCheckAsk({ ...base, engineDead: false, want: false }), "none");
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
