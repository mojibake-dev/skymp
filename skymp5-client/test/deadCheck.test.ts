// The view's dead check (src/sync/deadCheck.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { deadCheckAsk } from "../src/sync/deadCheck";

const base: Parameters<typeof deadCheckAsk>[0] = { engineDead: false, want: false, simulatedHere: false, bind: false, ownDeathMsAgo: undefined, wordWaitMs: 3000 };

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
