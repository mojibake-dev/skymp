// How long a copy's fed sample stays fresh on its engine, through the path a
// relayed sample takes on a seat that does not host the actor
// (src/sync/movementApply.ts: applyMovement, then the puppet's feed to
// setPuppetLocomotion). The platform module is a stand-in loaded under its
// name; it keeps each feed's arguments. The engine's own reading of a feed is
// the movement-step detour's (skyrim-platform PuppetHooks.cpp): a sample older
// than its window feeds Speed 0 and turns nothing, and a feed that names no
// window keeps Puppets.h kLocomotionStaleMs, 400 ms.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { Movement } from "../src/sync/movement";

const PUPPET = 0x10e2b6;
const ENGINE_DEFAULT_WINDOW_MS = 400;

// every property of the stand-in a function that returns the stand-in, so
// module-level reads load and a call made through it does nothing
const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
const stub = (own: Record<string, unknown>): any =>
  new Proxy(own, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

// the copy stands where its last sample put it, in the record's worldspace,
// with its 3D loaded and alive
let at: [number, number, number] = [0, 0, 0];
const world = stub({ getFormID: () => 0x3c });
const copy = stub({
  getFormID: () => PUPPET,
  isDead: () => false,
  is3DLoaded: () => true,
  getWorldSpace: () => world,
  getPositionX: () => at[0],
  getPositionY: () => at[1],
  getPositionZ: () => at[2],
  getAngleZ: () => 0,
});
const feeds: Array<{ at: number; args: unknown[] }> = [];
let now = 1_000_000;
const platform = stub({
  settings: {},
  Actor: stub({ from: (r: unknown) => (r === copy ? copy : null) }),
  Game: stub({ getPlayer: () => null }),
  setPuppetLocomotion: (...args: unknown[]) => {
    feeds.push({ at: now, args });
    return true;
  },
});

const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { applyMovement } = require("../src/sync/movementApply") as typeof import("../src/sync/movementApply");
const { SpApiInteractor } = require("../src/services/spApiInteractor") as typeof import("../src/services/spApiInteractor");
(SpApiInteractor as any).getControllerInstance = () => ({ emitter: { emit: () => {} } });

const realNow = Date.now;
Date.now = () => now;

// a walking sample at `x` along the road, the host's walk speed
const sample = (x: number): Movement => ({
  worldOrCell: 0x3c, pos: [x, 0, 0], rot: [0, 0, 90],
  runMode: "Walking", direction: 0, isInJumpState: false, isSneaking: false, isBlocking: false,
  isWeapDrawn: false, isDead: false, healthPercentage: 1, speed: 80.1, turnRate: 0,
});

// the copy's samples `gapMs` apart, each applied as the view applies a
// changed record; the feeds they made
const walk = (count: number, gapMs: number, from = 0): Array<{ at: number; args: unknown[] }> => {
  const start = feeds.length;
  for (let i = 0; i < count; ++i) {
    const x = from + i * 40;
    applyMovement(copy, sample(x), false, true, false);
    at = [x, 0, 0];
    now += gapMs;
  }
  now -= gapMs;
  return feeds.slice(start);
};
const windowOf = (feed: { args: unknown[] }): number =>
  typeof feed.args[5] === "number" ? (feed.args[5] as number) : ENGINE_DEFAULT_WINDOW_MS;

test("a far copy walking on 508 ms samples stays fed between them, its speed and its turn", () => {
  const fed = walk(6, 508, 1000);
  assert.equal(fed.length, 6);
  // past its first sample, each feed's window covers the gap to the next one
  for (let i = 1; i < fed.length - 1; ++i) {
    const gap = fed[i + 1].at - fed[i].at;
    assert.ok(windowOf(fed[i]) >= gap, `feed ${i}: a ${windowOf(fed[i])} ms window, the next sample ${gap} ms later`);
  }
  assert.ok((fed[1].args[1] as number) > 0, "the walk's speed is fed");
});

test("a far copy whose host stops sending goes still within two of its own gaps", () => {
  const fed = walk(6, 508, 5000);
  const last = fed[fed.length - 1];
  assert.ok(windowOf(last) <= 2 * 508, `a ${windowOf(last)} ms window`);
});

test("a near copy keeps the engine's 400 ms window on 140 ms samples", () => {
  const fed = walk(6, 140, 9000);
  for (const f of fed.slice(1)) {
    assert.equal(windowOf(f), ENGINE_DEFAULT_WINDOW_MS);
  }
});

test("a copy's first sample after a long pause is not fresh for the pause's length", () => {
  walk(3, 508, 13000);
  now += 10_000;
  const [resumed] = walk(1, 508, 14000);
  assert.ok(windowOf(resumed) <= 1000, `a ${windowOf(resumed)} ms window`);
});

test.after(() => {
  Date.now = realNow;
});
