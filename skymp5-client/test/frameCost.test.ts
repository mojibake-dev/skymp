// The per-frame work's bounds (src/sync/frameCost.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { dueSamples, movementApplyDue, distanceBetween, hostedSampleRateMs, Timed, LastWritten, RefSweep, appliedClock } from "../src/sync/frameCost";

test("45 hosted actors due at once are sampled 8 a frame, each within 6 frames", () => {
  const ids = Array.from({ length: 45 }, (_, i) => 0x1000 + i);
  const last = new Map<number, number>();
  let now = 1000000;
  const sampledAt = new Map<number, number>();
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
  const at = (x: number, inCombat = false, worldOrCell = 0x3c) => ({ pos: [x, 0, 0], worldOrCell, inCombat });
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
  const cell = new Timed<number>();
  const read = () => { reads += 1; return 0x3c; };
  assert.strictEqual(cell.get(0, 250, read), 0x3c);
  assert.strictEqual(cell.get(100, 250, read), 0x3c);
  assert.strictEqual(cell.get(249, 250, read), 0x3c);
  assert.strictEqual(reads, 1);
  cell.get(250, 250, read);
  assert.strictEqual(reads, 2);
});

test("an engine write of an unchanged value is skipped until its reassert", () => {
  const open = new LastWritten<boolean>();
  assert.strictEqual(open.due(true, 0, 1000), true);
  assert.strictEqual(open.due(true, 16, 1000), false);
  assert.strictEqual(open.due(false, 32, 1000), true);
  assert.strictEqual(open.due(false, 500, 1000), false);
  assert.strictEqual(open.due(false, 1032, 1000), true);
  const pitch = new LastWritten<number>();
  const near = (a: number, b: number) => Math.abs(a - b) < 1e-4;
  assert.strictEqual(pitch.due(0.5, 0, 1000, near), true);
  assert.strictEqual(pitch.due(0.50001, 16, 1000, near), false);
  assert.strictEqual(pitch.due(0.6, 32, 1000, near), true);
});

// frames of `dtMs` over the sweep: each frame's ids, and the first frame each id was read
const sweep = (s: RefSweep, ids: number[], frames: number, dtMs: number, start = 0) => {
  const reads: number[][] = [];
  const first = new Map<number, number>();
  for (let f = 0; f < frames; f += 1) {
    const slice = s.next(() => ids, start + f * dtMs);
    reads.push(slice);
    for (const id of slice) {
      if (!first.has(id)) {
        first.set(id, f);
      }
    }
  }
  return { reads, first };
};

test("1024 engine refs at 60 fps: no frame reads more than 64, and every ref is read within one 500 ms pass", () => {
  const ids = Array.from({ length: 1024 }, (_, i) => 0x10000 + i);
  const { reads, first } = sweep(new RefSweep(500, 64), ids, 40, 1000 / 60);
  for (const slice of reads) {
    assert.ok(slice.length <= 64, `a frame read ${slice.length}`);
  }
  assert.strictEqual(first.size, 1024);
  for (const f of first.values()) {
    assert.ok(f * (1000 / 60) <= 500 + 1000 / 60, `a ref first read at frame ${f}`);
  }
});

test("a slow frame reads no more than its share; a pass still ends", () => {
  const ids = Array.from({ length: 1024 }, (_, i) => i + 1);
  const { reads, first } = sweep(new RefSweep(500, 64), ids, 20, 200);
  assert.ok(reads.every((slice) => slice.length <= 64));
  assert.strictEqual(first.size, 1024);
});

test("a few refs are spread too: 10 refs at 60 fps, at most one a frame, all within the pass", () => {
  const ids = Array.from({ length: 10 }, (_, i) => i + 1);
  const { reads, first } = sweep(new RefSweep(500, 64), ids, 31, 1000 / 60);
  assert.ok(reads.every((slice) => slice.length <= 1));
  assert.strictEqual(first.size, 10);
});

test("a ref added during a pass is read in the next one; an empty set reads nothing", () => {
  const s = new RefSweep(500, 64);
  const ids = Array.from({ length: 300 }, (_, i) => i + 1);
  const seen = new Set<number>();
  let t = 0;
  for (let f = 0; f < 5; f += 1, t += 1000 / 60) {
    s.next(() => ids, t).forEach((id) => seen.add(id));
  }
  ids.push(9999);
  for (let f = 0; f < 70; f += 1, t += 1000 / 60) {
    s.next(() => ids, t).forEach((id) => seen.add(id));
  }
  assert.ok(seen.has(9999));
  assert.deepStrictEqual(new RefSweep(500, 64).next(() => [], 0), []);
});

test("views made in one frame never all fall due in one frame at the far rate", () => {
  const ids = Array.from({ length: 40 }, (_, i) => 0x1a600 + i * 7);
  const last = new Map<number, number>();
  const dt = 1000 / 60;
  let most = 0;
  for (let f = 0; f < 120; f += 1) {
    const now = 1000000 + f * dt;
    let due = 0;
    for (const id of ids) {
      const at = last.get(id);
      if (at === undefined || movementApplyDue({ changed: true, distance: 5000, nearUnits: 2048, sinceLastApplyMs: now - at,
        farRateMs: 500, reassertMs: 2000 })) {
        if (at !== undefined) {
          due += 1;
        }
        last.set(id, appliedClock(now, at === undefined, id, 500));
      }
    }
    most = Math.max(most, due);
  }
  assert.ok(most <= ids.length / 4, `${most} of ${ids.length} views fell due in one frame`);
});

test("a view's first clock is offset within the far rate, the same for the same id, and later ones are the apply's", () => {
  for (const id of [0x14, 0x1a600, 0xff000e41]) {
    const first = appliedClock(10000, true, id, 500);
    assert.ok(first <= 10000 && first > 10000 - 500, `${id.toString(16)}: ${first}`);
    assert.strictEqual(appliedClock(10000, true, id, 500), first);
    assert.strictEqual(appliedClock(10000, false, id, 500), 10000);
  }
});
