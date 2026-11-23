// A grip of the other player's figure, its throw, and a grip again seconds
// later, through the sample path's undriven rule (src/sync/drivenParts.ts
// sampleStep, UndrivenSkeletons), with the reads of a beast-race figure: 23
// parts in walk order, the COM first and the tail's five bones at 7 to 11.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { Point } from "../src/sync/boneAnchor";
import { PartsSample, SampleStep, UndrivenSkeletons, WalkPart, sampleStep, UNDRIVEN_WAIT_MS } from "../src/sync/drivenParts";

const REACH = 512;
const TAIL = [7, 8, 9, 10, 11];
const FIGURE = 0xff000de9;
const REMADE = 0xff000e0e;

const walk = (motion: (i: number) => number): WalkPart[] =>
    Array.from({ length: 23 }, (_, i) => ({
        node: i === 0 ? "NPC COM [COM ]" : TAIL.includes(i) ? `TailBone0${i - 6}` : `NPC part ${i}`,
        motion: motion(i),
    }));

// the grab's read: the body dynamic, the tail keyframed (2 and 4)
const grabRead = walk((i) => (TAIL.includes(i) ? 4 : 2));
// a read mid-hold with every part dynamic
const allDynamic = walk(() => 2);
// a read where every part is keyframed, a fresh grip's first frames
const allKeyframed = walk(() => 4);

const COM: Point = [1000, 2000, 300];
// each body part within 230 units of the COM, each tail part `tail(i)` units
// out along x
const read = (tail: (i: number) => number): Point[] =>
    Array.from({ length: 23 }, (_, i) =>
        TAIL.includes(i) ? [COM[0] + tail(i), COM[1], COM[2]] : [COM[0], COM[1], COM[2] + 10 * i],
    );
// the tail read about 700 units out while held, 520 to 548 in flight (parts 8
// to 11), 677 at a grip's first sample
const held = read(() => 697);
const thrown = read((i) => [505, 520.4, 531, 538, 548][i - 7]);
const gripAgain = read((i) => (i === 7 ? 677.5 : 600));

const sample = (u: UndrivenSkeletons, localId: number, positions: Point[], over: Partial<PartsSample> = {}): SampleStep =>
    sampleStep({
        root: COM,
        positions,
        anchored: false,
        known: u.of(localId),
        learnNow: () => u.of(localId),
        pastSince: 0,
        now: 0,
        reach: REACH,
        ...over,
    });

const within = (s: SampleStep) => {
    assert.strictEqual(s.step, "send");
    if (s.step !== "send") {
        return;
    }
    s.positions.forEach((p, i) => {
        const d = Math.hypot(p[0] - s.pos[0], p[1] - s.pos[1], p[2] - s.pos[2]);
        assert.ok(d <= REACH, `part ${i} lies ${d.toFixed(1)} from the sample's pos`);
    });
};

test("the first grip holds her, and its throw is sent to her rest though a read mid-hold had every part dynamic", () => {
    const u = new UndrivenSkeletons();
    u.learn(FIGURE, grabRead, true);
    within(sample(u, FIGURE, held));
    u.learn(FIGURE, allDynamic, true);
    within(sample(u, FIGURE, thrown));
});

test("a grip again seconds after the throw holds her: its first sample, her tail 677 units out, is sent", () => {
    const u = new UndrivenSkeletons();
    u.learn(FIGURE, grabRead, true);
    within(sample(u, FIGURE, thrown));
    u.learn(FIGURE, allDynamic, true);
    within(sample(u, FIGURE, gripAgain));
});

test("a grip of her figure re-made under another id is the same skeleton: its first sample is sent", () => {
    const u = new UndrivenSkeletons();
    u.learn(FIGURE, grabRead, true);
    const step = sample(u, REMADE, gripAgain, {
        learnNow: () => {
            u.learn(REMADE, allDynamic, true);
            return u.of(REMADE);
        },
    });
    within(step);
});

test("a fresh session's first grip, every part keyframed at the grab: the sample waits for the ragdoll's read, then is sent", () => {
    const u = new UndrivenSkeletons();
    u.learn(FIGURE, allKeyframed, true);
    const first = sample(u, FIGURE, gripAgain, { now: 5000 });
    assert.deepStrictEqual(first, { step: "wait", pastSince: 5000 });
    const next = sample(u, FIGURE, gripAgain, {
        now: 5300,
        pastSince: 5000,
        learnNow: () => {
            u.learn(FIGURE, grabRead, true);
            return u.of(FIGURE);
        },
    });
    within(next);
});

test("a body torn past reach in a part the engine drives still drops its row once the wait is over", () => {
    const u = new UndrivenSkeletons();
    const torn = read(() => 0);
    torn[3] = [COM[0] + 600, COM[1], COM[2]];
    const step = sample(u, FIGURE, torn, { now: 5000 + UNDRIVEN_WAIT_MS, pastSince: 5000 });
    assert.strictEqual(step.step, "drop");
    if (step.step === "drop") {
        assert.strictEqual(step.past, 3);
    }
});
