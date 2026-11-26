// A container's prompt reads "Empty" when the engine counts nothing in it (TESObjectCONT::GetActivateText, the count
// of FUN_1402e1fb0: the base's list while the reference has no changes, its changes' count once it has some;
// HYPOTHESIS, decompiled). Every placement of a base shares that base's list (a food barrel's base is the base of
// hundreds), so a container's apply of the server's record lands on that reference alone: its base's list is left as
// the game files hold it, and the record is laid as a diff against what the engine counts there, the base's items and
// the reference's changes. The platform module is a stand-in loaded under its name, as in
// test/shoutClaimUpdate.test.ts; the engine here is this file's: a base list per base, a change list per reference,
// and the engine's own count of a reference as their sum.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type * as ModelApplyModule from "../src/view/modelApplyUtils";

// a food barrel's base (a CONT) and two barrels of it; the items are stand-in forms
const BARREL_BASE = 0x845;
const BARREL_OPENED = 0x10a01;
const BARREL_UNTOUCHED = 0x10a02;
const APPLE = 0x1001;
const BREAD = 0x1002;
const CABBAGE = 0x1003;
const MISC = 32;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});

// the stand-in engine
const bases = new Map<number, Array<{ baseId: number; count: number }>>();
const changes = new Map<number, Map<number, number>>();
const refBase = new Map<number, number>();
const resets: number[] = [];
let rollOnFirstWrite: number | null = null;

const form = (id: number) => ({ getFormID: () => id, getType: () => MISC, getName: () => "" });
const reference = (id: number) => ({
  getFormID: () => id,
  getBaseObject: () => form(refBase.get(id) ?? 0),
  removeAllItems: () => {
    const own = changes.get(id) ?? new Map<number, number>();
    (bases.get(refBase.get(id) ?? 0) ?? []).forEach((e) => own.set(e.baseId, -e.count));
    changes.set(id, own);
  },
});
const write = (refId: number, baseId: number, count: number) => {
  if (!changes.has(refId)) {
    changes.set(refId, new Map());
    // the engine laying its own leveled roll into a reference's inventory at its first write, when a test asks for it
    if (rollOnFirstWrite !== null) {
      changes.get(refId)!.set(rollOnFirstWrite, 1);
    }
  }
  const own = changes.get(refId)!;
  own.set(baseId, (own.get(baseId) ?? 0) + count);
};
// the engine's own count of a reference: its base's list and its changes
const counted = (refId: number): Record<number, number> => {
  const out: Record<number, number> = {};
  (bases.get(refBase.get(refId) ?? 0) ?? []).forEach((e) => (out[e.baseId] = (out[e.baseId] ?? 0) + e.count));
  (changes.get(refId) ?? new Map()).forEach((n, b) => (out[b] = (out[b] ?? 0) + n));
  Object.keys(out).forEach((k) => out[+k] === 0 && delete out[+k]);
  return out;
};

// the platform's classes the apply tests against (its argument printer's instanceof)
class StandInForm {}
class StandInReference {
  static from(f: unknown) {
    return f ?? null;
  }
}

const platform = new Proxy(
  {
    storage: {},
    Form: StandInForm,
    FormType: { Misc: MISC, Potion: 46, Ingredient: 30, Container: 28 },
    Game: { getFormEx: (id: number) => form(id) },
    Ammo: { from: () => null },
    Actor: { from: () => null },
    ObjectReference: StandInReference,
    // the fixed native's answer: the base's own list (leveled entries left to their rolls in the changes)
    getContainer: (baseId: number) => (bases.get(baseId) ?? []).map((e) => ({ ...e })),
    getExtraContainerChanges: (refId: number) =>
      [...(changes.get(refId) ?? new Map()).entries()].map(([baseId, countDelta]) => ({ baseId, countDelta, extendDataList: [] })),
    TESModPlatform: {
      resetContainer: (base: { getFormID(): number }) => {
        resets.push(base.getFormID());
        bases.set(base.getFormID(), []);
      },
      addItemEx: (refr: { getFormID(): number }, f: { getFormID(): number }, count: number) =>
        write(refr.getFormID(), f.getFormID(), count),
      pushWornState: () => undefined,
    },
  },
  { get: (t, k) => (k in t ? (t as any)[k] : anything) },
);
const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { ModelApplyUtils } = require("../src/view/modelApplyUtils") as typeof ModelApplyModule;

// two barrels of one base whose list holds two apples, neither changed yet
const world = () => {
  bases.clear();
  changes.clear();
  refBase.clear();
  resets.length = 0;
  rollOnFirstWrite = null;
  (platform as any).storage = {};
  bases.set(BARREL_BASE, [{ baseId: APPLE, count: 2 }]);
  refBase.set(BARREL_OPENED, BARREL_BASE);
  refBase.set(BARREL_UNTOUCHED, BARREL_BASE);
};
const apply = (refId: number, entries: Array<{ baseId: number; count: number }>) =>
  ModelApplyUtils.applyModelInventory(reference(refId) as any, { entries });

test("a barrel's record lands on that barrel alone: an untouched barrel of its base still counts its apples", () => {
  world();
  apply(BARREL_OPENED, [{ baseId: APPLE, count: 2 }]);
  assert.deepEqual(resets, []);
  assert.deepEqual(counted(BARREL_UNTOUCHED), { [APPLE]: 2 });
});

test("an opened barrel holds its record's items once each, laid as a diff against its base's apples", () => {
  world();
  apply(BARREL_OPENED, [{ baseId: APPLE, count: 2 }, { baseId: BREAD, count: 1 }]);
  assert.deepEqual(counted(BARREL_OPENED), { [APPLE]: 2, [BREAD]: 1 });
  assert.deepEqual(counted(BARREL_UNTOUCHED), { [APPLE]: 2 });
});

test("a barrel a player emptied counts nothing on this seat, its base's apples taken off that barrel alone", () => {
  world();
  apply(BARREL_OPENED, []);
  assert.deepEqual(counted(BARREL_OPENED), {});
  assert.deepEqual(counted(BARREL_UNTOUCHED), { [APPLE]: 2 });
});

test("a leveled roll the engine lays at a barrel's first write is taken off again, so the barrel holds its record", () => {
  world();
  rollOnFirstWrite = CABBAGE;
  apply(BARREL_OPENED, [{ baseId: APPLE, count: 2 }, { baseId: BREAD, count: 1 }]);
  assert.deepEqual(counted(BARREL_OPENED), { [APPLE]: 2, [BREAD]: 1 });
});
