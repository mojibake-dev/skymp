// The record laid on the player's game when the game holds a surplus of arrows, through applyInventory itself
// (src/sync/inventory.ts, the player's apply loop's call). A take-all from a chest whose copy held its arrows many
// times over put about a thousand more in the player's game than its record holds; the take was refused and the
// record laid again. A difference of more than a thousand landed as one step of +1, its sign dropped, so every apply
// added an arrow and the surplus never left; a smaller one left one arrow per call, each its own notification. The
// difference lands whole, in one call, either way. The platform module is a stand-in loaded under its name, as in
// test/shoutClaimUpdate.test.ts; the engine's container is this file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type * as InventoryModule from "../src/sync/inventory";

const PLAYER = 0x14;
const DWARVEN_ARROW = 0x139bc;
const IRON_SWORD = 0x12eb7;
const AMMO_TYPE = 42;
const WEAPON_TYPE = 41;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
// the player's engine container: what the game holds of each base beyond the base container (none)
let held = new Map<number, number>();
const calls: Array<[number, number]> = [];
const forms = new Map<number, unknown>([
  [DWARVEN_ARROW, { getFormID: () => DWARVEN_ARROW, getType: () => AMMO_TYPE, getName: () => "Dwarven Arrow" }],
  [IRON_SWORD, { getFormID: () => IRON_SWORD, getType: () => WEAPON_TYPE, getName: () => "Iron Sword" }],
]);
const platform = new Proxy({
  storage: {},
  printConsole: () => {},
  // classes of their own, so the apply's argument printer's instanceof tests answer no for a number
  ObjectReference: class {},
  Form: class {},
  FormType: { Misc: 32, Potion: 46, Ingredient: 30, Ammo: AMMO_TYPE, Weapon: WEAPON_TYPE },
  Ammo: { from: (f: { getType?: () => number } | null) => (f && f.getType && f.getType() === AMMO_TYPE ? f : null) },
  Game: { getFormEx: (id: number) => forms.get(id) ?? null },
  getContainer: () => [],
  getExtraContainerChanges: () => Array.from(held, ([baseId, countDelta]) => ({ baseId, countDelta, extendDataList: [] })),
  TESModPlatform: {
    addItemEx: (_refr: unknown, form: { getFormID: () => number }, count: number) => { calls.push([form.getFormID(), count]); },
    pushWornState: () => {},
    resetContainer: () => {},
  },
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { applyInventory } = require("../src/sync/inventory") as typeof InventoryModule;

const player = new Proxy({ getFormID: () => PLAYER, getBaseObject: () => ({ getFormID: () => 0x7 }) }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

// the player's own apply, as its loop calls it: no crash protection, worn ignored, the base reset once
const apply = (game: Array<[number, number]>, record: Array<[number, number]>) => {
  held = new Map(game);
  calls.length = 0;
  applyInventory(player as any, { entries: record.map(([baseId, count]) => ({ baseId, count })) }, false, true, true);
  return calls.slice();
};

test("a surplus of more than a thousand arrows over the record leaves in one call", () => {
  assert.deepEqual(apply([[DWARVEN_ARROW, 1122]], [[DWARVEN_ARROW, 20]]), [[DWARVEN_ARROW, -1102]]);
});

test("a smaller surplus of arrows leaves in one call, not one arrow at a time", () => {
  assert.deepEqual(apply([[DWARVEN_ARROW, 122]], [[DWARVEN_ARROW, 20]]), [[DWARVEN_ARROW, -102]]);
});

test("a deficit of more than a thousand of a weapon lands whole in one call", () => {
  assert.deepEqual(apply([[IRON_SWORD, 0]], [[IRON_SWORD, 1500]]), [[IRON_SWORD, 1500]]);
});

test("a deficit of arrows lands in one call, as before", () => {
  assert.deepEqual(apply([[DWARVEN_ARROW, 20]], [[DWARVEN_ARROW, 80]]), [[DWARVEN_ARROW, 60]]);
});
