// The follower's segment velocities (src/sync/propFollow.ts segmentVelocities), the feed the
// platform's drive adds to its correction: compiled by the package's own tsc into build/test and run
// with plain node (yarn test).
"use strict";
const assert = require("assert");
const f = require("../build/test/propFollow.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

// a unit quaternion of a turn about a unit axis, lanes x, y, z, w
const turn = (axis, degrees) => {
  const h = (degrees * Math.PI) / 360;
  const s = Math.sin(h);
  return [axis[0] * s, axis[1] * s, axis[2] * s, Math.cos(h)];
};
// the Hamilton product a b (b first), as the platform's PropBodies.cpp Mul
const mul = (a, b) => [
  a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
  a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
  a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
];
// the platform's PropBodies::Advanced on a rotation: q turned by ang (radians a second) over t seconds
const advanced = (q, ang, t) => {
  const spin = ang.map((v) => v * t);
  const angle = Math.hypot(...spin);
  const s = Math.sin(angle / 2) / angle;
  return mul([spin[0] * s, spin[1] * s, spin[2] * s, Math.cos(angle / 2)], q);
};
const sample = (tMs, bones) => ({
  epoch: 1, seq: 1, tMs, pos: [0, 0, 0], rot: [0, 0, 0], lin: [0, 0, 0], ang: [0, 0, 0], asleep: false, held: false, bones,
});
const near = (a, b, eps = 1e-9) => a.length === b.length && a.every((v, i) => Math.abs(v - b[i]) < eps);

test("a still segment feeds zeros, so a body at rest is driven as with no feed", () => {
  const bone = { pos: [1, 2, 3], quat: turn([0, 0, 1], 30) };
  const v = f.segmentVelocities(sample(0, [bone]), sample(100, [bone]));
  assert.deepStrictEqual(v[0].lin, [0, 0, 0]);
  assert.ok(near(v[0].ang, [0, 0, 0]), v[0].ang);
});

test("a moving part's velocity is its position's change over the segment's time", () => {
  const v = f.segmentVelocities(sample(0, [{ pos: [0, 0, 0], quat: [0, 0, 0, 1] }]),
    sample(100, [{ pos: [150, 0, -20], quat: [0, 0, 0, 1] }]));
  assert.ok(near(v[0].lin, [1500, 0, -200]), v[0].lin);
});

test("a turn is its axis times its angle over the segment's time, from any start", () => {
  const a = turn([0, 1, 0], 40);
  const b = mul(turn([0, 0, 1], 90), a);
  const v = f.segmentVelocities(sample(0, [{ pos: [0, 0, 0], quat: a }]), sample(1000, [{ pos: [0, 0, 0], quat: b }]));
  assert.ok(near(v[0].ang, [0, 0, Math.PI / 2]), v[0].ang);
  // the sign of a quaternion is no turn: -b is b
  const w = f.segmentVelocities(sample(0, [{ pos: [0, 0, 0], quat: a }]),
    sample(1000, [{ pos: [0, 0, 0], quat: b.map((x) => -x) }]));
  assert.ok(near(w[0].ang, v[0].ang), w[0].ang);
});

test("the platform's advance of a part along its fed turn lands on the segment's end", () => {
  const a = turn([1, 0, 0], 25);
  const b = mul(turn([0.6, 0, 0.8], 50), a);
  const v = f.segmentVelocities(sample(0, [{ pos: [0, 0, 0], quat: a }]), sample(200, [{ pos: [0, 0, 0], quat: b }]));
  assert.ok(near(advanced(a, v[0].ang, 0.2), b), [advanced(a, v[0].ang, 0.2), b]);
});

test("a jump no sample could carry, a segment of no time, or another count feeds nothing", () => {
  const still = { pos: [0, 0, 0], quat: [0, 0, 0, 1] };
  // 700 units in 100 ms is past the wire's 6000 a second: a teleport, not motion
  assert.strictEqual(f.segmentVelocities(sample(0, [still]), sample(100, [{ pos: [700, 0, 0], quat: [0, 0, 0, 1] }])), undefined);
  assert.strictEqual(f.SEGMENT_LIN_MAX_UNITS_PER_S, 6000);
  // a dry frame's pair is one sample: no time
  assert.strictEqual(f.segmentVelocities(sample(100, [still]), sample(100, [still])), undefined);
  assert.strictEqual(f.segmentVelocities(sample(0, [still]), sample(100, [still, still])), undefined);
  assert.strictEqual(f.segmentVelocities(sample(0, []), sample(100, [])), undefined);
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
