// A puppet's pose from its host's sample, through applyMovement itself (src/sync/movementApply.ts). A copy its engine
// holds in a furniture (a draugr lying in its coffin, a resident on a bench) is its furniture's to place: the host's
// actor leaves the furniture on the host's engine, and the copy here leaves it when the host's heading turns it, by the
// absolute turn the engine's MoveTo makes (Papyrus SetAngle's body), as every puppet was turned before the movement
// step's turn. The step's turn, its rate fed to TurnDelta, is for a copy no furniture holds. The platform module is a
// stand-in loaded under its name, as in test/shoutClaimUpdate.test.ts; the copy's engine reads are this file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type * as MovementApplyModule from "../src/sync/movementApply";
import type { Movement } from "../src/sync/movement";

const BOSS = 0x9bcd6;
const COFFIN = 0x480b7;
const SANCTUM = 0x371dd;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});

// the copy as its engine has it: in the Sanctum, facing 0 degrees, in its coffin or out of any furniture
const copy = (inFurniture: boolean) => {
  const angles: number[][] = [];
  const own = {
    getFormID: () => BOSS,
    getWorldSpace: () => null,
    getParentCell: () => ({ getFormID: () => SANCTUM }),
    is3DLoaded: () => true,
    isDead: () => false,
    getPositionX: () => -7216,
    getPositionY: () => -613,
    getPositionZ: () => 604,
    getAngleX: () => 0,
    getAngleY: () => 0,
    getAngleZ: () => 0,
    getSitState: () => 0,
    getFurnitureReference: () => (inFurniture ? { getFormID: () => COFFIN } : null),
    getActorValue: () => 0,
    setAngle: (x: number, y: number, z: number) => { angles.push([x, y, z]); },
  };
  const actor = new Proxy(own, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  return { actor, angles };
};

const fed: unknown[][] = [];
let current: unknown = null;
const platform = new Proxy({
  storage: {},
  settings: { "skymp5-client": {} },
  Actor: { from: (f: unknown) => (f !== null && f === current ? f : null) },
  Game: { getPlayer: () => ({ getPositionX: () => -7300, getPositionY: () => -600, getPositionZ: () => 604 }) },
  setPuppetLocomotion: (...args: unknown[]) => { fed.push(args); return true; },
  getBodyClass: () => null,
  printConsole: () => {},
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { applyMovement } = require("../src/sync/movementApply") as typeof MovementApplyModule;

// the host's sample: the boss risen a step out of the coffin, turned to 90 degrees, turning at 120 degrees a second
const sample = (headingDeg: number): Movement => ({
  worldOrCell: SANCTUM,
  pos: [-7180, -560, 604],
  rot: [0, 0, headingDeg],
  runMode: "Walking",
  direction: 0,
  isInJumpState: false,
  isSneaking: false,
  isBlocking: false,
  isWeapDrawn: false,
  isDead: false,
  healthPercentage: 1,
  speed: 60,
  turnRate: 120,
});

const apply = (inFurniture: boolean, headingDeg: number) => {
  const c = copy(inFurniture);
  current = c.actor;
  fed.length = 0;
  applyMovement(c.actor as any, sample(headingDeg), false, true);
  return { angles: c.angles, fed: fed.map((a) => a.slice()) };
};

test("a copy its engine holds in its coffin takes the host's turn as an absolute turn, and is fed no turn of its own", () => {
  const { angles, fed } = apply(true, 90);
  assert.deepEqual(angles, [[0, 0, 90]], "the absolute turn that takes it out of the furniture");
  assert.equal(fed.length, 1);
  assert.equal(fed[0][3], 0, "no turn rate to its graph's TurnDelta");
  assert.equal(fed[0][4], undefined, "no heading for the movement step to turn it toward");
  assert.equal(typeof fed[0][5], "number", "the sample's freshness window as for every copy");
});

test("a copy in its coffin whose host's heading holds is not turned", () => {
  const { angles } = apply(true, 0.5);
  assert.deepEqual(angles, []);
});

test("a copy out of any furniture is turned by its movement step at the host's rate, with no absolute turn", () => {
  const { angles, fed } = apply(false, 90);
  assert.deepEqual(angles, []);
  assert.equal(fed.length, 1);
  assert.equal(fed[0][3], 120);
  assert.equal(fed[0][4], 90);
  assert.equal(typeof fed[0][5], "number");
});
