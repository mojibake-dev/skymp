// The own player's death beside a killmove (src/sync/playerDeath.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { playerDeathStep } from "../src/sync/playerDeath";

const MAX = 10000;

test("a lethal hit during a killmove pushes nothing until the killmove ends", () => {
  assert.strictEqual(playerDeathStep({ inKillMove: true, waitedMs: 0, maxWaitMs: MAX }), "wait");
  assert.strictEqual(playerDeathStep({ inKillMove: true, waitedMs: 2500, maxWaitMs: MAX }), "wait");
  assert.strictEqual(playerDeathStep({ inKillMove: false, waitedMs: 2600, maxWaitMs: MAX }), "die");
});

test("a death with no killmove is applied at once", () => {
  assert.strictEqual(playerDeathStep({ inKillMove: false, waitedMs: 0, maxWaitMs: MAX }), "die");
});

test("a killmove that never ends holds the death no longer than the most wait", () => {
  assert.strictEqual(playerDeathStep({ inKillMove: true, waitedMs: MAX, maxWaitMs: MAX }), "die");
});
