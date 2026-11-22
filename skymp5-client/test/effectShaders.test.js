// The shader pass's choice (src/sync/effectShaders.ts), JS state alone: compiled by the
// package's own tsc into build/test and run with plain node (yarn test).
"use strict";
const assert = require("assert");
const s = require("../build/test/effectShaders.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

const swirl = 0xdc209;

test("the gripper's game, whose engine runs the cloak's effect on the gripped figure, plays no second swirl", () => {
  const pass = s.shaderPass([{ shader: swirl, engineRuns: true, seconds: 1 }], new Set());
  assert.deepStrictEqual(pass.play, []);
  assert.deepStrictEqual(pass.stop, []);
});

test("the gripped player's own game, whose engine never got the effect, plays the swirl for the server's seconds", () => {
  const pass = s.shaderPass([{ shader: swirl, engineRuns: false, seconds: 1 }], new Set());
  assert.deepStrictEqual(pass.play, [{ shader: swirl, seconds: 1 }]);
  assert.ok(pass.shown.has(swirl));
});

test("a swirl this game played stays while the engine runs the effect, and stops once the server's set drops it", () => {
  const first = s.shaderPass([{ shader: swirl, engineRuns: false, seconds: 1 }], new Set());
  const engineNow = s.shaderPass([{ shader: swirl, engineRuns: true, seconds: 1 }], first.shown);
  assert.deepStrictEqual(engineNow.play, []);
  assert.deepStrictEqual(engineNow.stop, []);
  const dropped = s.shaderPass([], engineNow.shown);
  assert.deepStrictEqual(dropped.stop, [swirl]);
});

test("an effect with no shader plays nothing and stops nothing", () => {
  const pass = s.shaderPass([{ shader: 0, engineRuns: false, seconds: 1 }], new Set([swirl]));
  assert.deepStrictEqual(pass.play, []);
  assert.deepStrictEqual(pass.stop, [swirl]);
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
