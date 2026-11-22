// The container events' baseline (src/sync/baseline.ts), JS state alone:
// compiled by the package's own tsc into build/test and run with plain node
// (yarn test).
"use strict";
const assert = require("assert");
const { advanceBaseline } = require("../build/test/baseline.js");

const cases = [];
const test = (name, fn) => cases.push([name, fn]);

// a stack is its base and its extras; the count is how many
const extras = (e) => JSON.stringify(Object.entries(e).filter(([k]) => k !== "count").sort());
const sameStack = (a, b) => extras(a) === extras(b);

// the event's report: the baseline less the game, a stack the game lacks
// positive, one it gained negative (getDiff's shape)
const diff = (baseline, game) => {
  const out = baseline.map((e) => ({ ...e }));
  for (const g of game) {
    const match = out.find((e) => sameStack(e, g));
    if (match) {
      match.count -= g.count;
    } else {
      out.push({ ...g, count: -g.count });
    }
  }
  return out.filter((e) => e.count !== 0);
};

// one burst: `events` events, each reporting what its handler finds against
// the baseline as the last event left it; the reports per stack
const burst = (record, game, events) => {
  let baseline = record;
  const reports = new Map();
  for (let i = 0; i < events; i += 1) {
    const reported = diff(baseline, game);
    for (const r of reported) {
      const key = extras(r);
      reports.set(key, (reports.get(key) || 0) + 1);
    }
    baseline = advanceBaseline(baseline, reported, sameStack);
  }
  return { reports, baseline };
};

test("a strip's burst of N events reports each stack once, owned and worn stacks included", () => {
  const record = [
    { baseId: 0x1397d, count: 1, worn: true },
    { baseId: 0x1397d, count: 113 },
    { baseId: 0xd2844, count: 1, worn: true },
    { baseId: 0xb9bd6, count: 1, ownerId: 0x9f25a },
    { baseId: 0xf, count: 72 },
  ];
  const { reports, baseline } = burst(record, [], 50);
  assert.strictEqual(reports.size, record.length);
  for (const n of reports.values()) {
    assert.strictEqual(n, 1);
  }
  assert.deepStrictEqual(baseline, []);
});

test("a take-all's burst reports each stack the game gained once, the faction's mark on each", () => {
  const record = [{ baseId: 0x3eae5, count: 4 }];
  const game = [
    { baseId: 0x3eae5, count: 9, ownerId: 0x267ea },
    { baseId: 0xa, count: 19, ownerId: 0x267ea },
    { baseId: 0xd2844, count: 1, ownerId: 0x267ea },
  ];
  const { reports, baseline } = burst(record, game, 21);
  // the plain stack left and the three marked ones came: four stacks, once each
  assert.strictEqual(reports.size, 4);
  for (const n of reports.values()) {
    assert.strictEqual(n, 1);
  }
  assert.deepStrictEqual(diff(baseline, game), []);
});

test("a stack's part moves alone, the rest of it kept", () => {
  const out = advanceBaseline([{ baseId: 0x3eadd, count: 6 }], [{ baseId: 0x3eadd, count: 2 }], sameStack);
  assert.deepStrictEqual(out, [{ baseId: 0x3eadd, count: 4 }]);
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
