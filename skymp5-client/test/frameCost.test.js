// The per-frame work's bounds (src/sync/frameCost.ts), JS state alone:
// compiled by the package's own tsc into build/test and run with plain node
// (yarn test).
"use strict";
const assert = require("assert");
const { dueSamples, movementApplyDue, distanceBetween, hostedSampleRateMs, Timed, LastWritten } = require("../build/test/frameCost.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

test("45 hosted actors due at once are sampled 8 a frame, each within 6 frames", () => {
  const ids = Array.from({ length: 45 }, (_, i) => 0x1000 + i);
  const last = new Map();
  let now = 1000000;
  const sampledAt = new Map();
  for (let frame = 0; frame < 6; frame += 1) {
    const due = dueSamples(ids, (id) => last.get(id), now, () => 130, 8);
    assert.ok(due.length <= 8);
    for (const id of due) {
      last.set(id, now);
      if (!sampledAt.has(id)) {
        sampledAt.set(id, frame);
      }
    }
    now += 100;
  }
  assert.strictEqual(sampledAt.size, 45);
  for (const frame of sampledAt.values()) {
    assert.ok(frame <= 5);
  }
});

test("an actor sampled within the send rate is not due; the longest waiting goes first", () => {
  const last = new Map([[1, 1000], [2, 700], [3, 990]]);
  assert.deepStrictEqual(dueSamples([1, 2, 3], (id) => last.get(id), 1100, () => 130, 8), [2]);
  assert.deepStrictEqual(dueSamples([1, 2, 3, 4], (id) => last.get(id), 1200, () => 130, 2), [4, 2]);
  // each actor at its own rate: 2 waits 400 ms, due at 130 but not at 500
  assert.deepStrictEqual(dueSamples([1, 2], (id) => last.get(id), 1100, (id) => (id === 2 ? 500 : 130), 8), []);
});

test("a hosted actor no other player is near is sampled at the far rate; near one, or in combat, at the send rate", () => {
  const base = { nearUnits: 2048, sendRateMs: 130, farRateMs: 500 };
  const at = (x, inCombat = false, worldOrCell = 0x3c) => ({ pos: [x, 0, 0], worldOrCell, inCombat });
  // the other player stands 300 units off: the send rate
  assert.strictEqual(hostedSampleRateMs({ ...base, last: at(0), others: [{ pos: [300, 0, 0], worldOrCell: 0x3c }] }), 130);
  // the other player is 5000 units off: the far rate
  assert.strictEqual(hostedSampleRateMs({ ...base, last: at(0), others: [{ pos: [5000, 0, 0], worldOrCell: 0x3c }] }), 500);
  // the other player is near in another world or cell: the far rate
  assert.strictEqual(hostedSampleRateMs({ ...base, last: at(0), others: [{ pos: [300, 0, 0], worldOrCell: 0x1a26f }] }), 500);
  // no other player at all: the far rate
  assert.strictEqual(hostedSampleRateMs({ ...base, last: at(0), others: [] }), 500);
  // in combat at its last sample, no other player near: the send rate
  assert.strictEqual(hostedSampleRateMs({ ...base, last: at(0, true), others: [] }), 130);
  // never sampled: the send rate
  assert.strictEqual(hostedSampleRateMs({ ...base, last: undefined, others: [] }), 130);
});

test("a new movement near the player applies at once; one far away at the far rate; any view at the reassert", () => {
  const base = { nearUnits: 2048, farRateMs: 500, reassertMs: 2000 };
  assert.strictEqual(movementApplyDue({ ...base, changed: true, distance: 300, sinceLastApplyMs: 16 }), true);
  assert.strictEqual(movementApplyDue({ ...base, changed: true, distance: 5000, sinceLastApplyMs: 130 }), false);
  assert.strictEqual(movementApplyDue({ ...base, changed: true, distance: 5000, sinceLastApplyMs: 500 }), true);
  assert.strictEqual(movementApplyDue({ ...base, changed: false, distance: 300, sinceLastApplyMs: 1999 }), false);
  assert.strictEqual(movementApplyDue({ ...base, changed: false, distance: 5000, sinceLastApplyMs: 2001 }), true);
  // an unknown distance is never near
  assert.strictEqual(movementApplyDue({ ...base, changed: true, distance: distanceBetween(undefined, [0, 0, 0]), sinceLastApplyMs: 16 }), false);
});

test("a value read through Timed is read once per its time to live", () => {
  let reads = 0;
  const cell = new Timed();
  const read = () => { reads += 1; return 0x3c; };
  assert.strictEqual(cell.get(0, 250, read), 0x3c);
  assert.strictEqual(cell.get(100, 250, read), 0x3c);
  assert.strictEqual(cell.get(249, 250, read), 0x3c);
  assert.strictEqual(reads, 1);
  cell.get(250, 250, read);
  assert.strictEqual(reads, 2);
});

test("an engine write of an unchanged value is skipped until its reassert", () => {
  const open = new LastWritten();
  assert.strictEqual(open.due(true, 0, 1000), true);
  assert.strictEqual(open.due(true, 16, 1000), false);
  assert.strictEqual(open.due(false, 32, 1000), true);
  assert.strictEqual(open.due(false, 500, 1000), false);
  assert.strictEqual(open.due(false, 1032, 1000), true);
  const pitch = new LastWritten();
  const near = (a, b) => Math.abs(a - b) < 1e-4;
  assert.strictEqual(pitch.due(0.5, 0, 1000, near), true);
  assert.strictEqual(pitch.due(0.50001, 16, 1000, near), false);
  assert.strictEqual(pitch.due(0.6, 32, 1000, near), true);
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
