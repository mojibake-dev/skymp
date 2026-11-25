// A burst of one container's records from the server within one tick, through RemoteServer's own UpdateProperty
// handler (src/services/services/remoteServer.ts). Each apply lays the difference between the record and the
// engine's container through TESModPlatform.addItemEx, which queues the change for the game thread, so a second apply
// in the same tick reads the container before the first one's changes land and lays the same difference again: a
// strip's or a take-all's burst left a chest's copy holding its arrows many times over. The burst lands once, as its
// newest record. The platform module is a stand-in loaded under its name, as in test/shoutClaimUpdate.test.ts; the
// update tick and the apply's engine half are this file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { RemoteServer as RemoteServerClass } from "../src/services/services/remoteServer";
import type { Inventory } from "../src/sync/inventory";

const CHEST = 0x267e8;
const OTHER_CHEST = 0x9dc64;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
// the next update's callbacks, run when the test says the tick has come
let updates: Array<() => void> = [];
const tick = () => {
  const due = updates;
  updates = [];
  due.forEach((fn) => fn());
};
const chests = new Map<number, unknown>([[CHEST, { getFormID: () => CHEST }], [OTHER_CHEST, { getFormID: () => OTHER_CHEST }]]);
const platform = new Proxy({
  storage: {},
  settings: { "skymp5-client": {} },
  once: (name: string, fn: () => void) => { if (name === "update") { updates.push(fn); } return anything; },
  on: () => anything,
  Game: { getFormEx: (id: number) => chests.get(id) ?? null },
  ObjectReference: { from: (f: unknown) => f ?? null },
  Ui: { isMenuOpen: () => false },
  printConsole: () => {},
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
// the apply's engine half, counted: which container, with which record
const applied: Array<{ id: number; arrows: number }> = [];
const { ModelApplyUtils } = require("../src/view/modelApplyUtils");
ModelApplyUtils.applyModelInventory = (refr: { getFormID: () => number }, inv: Inventory) => {
  applied.push({ id: refr.getFormID(), arrows: inv.entries.find((e) => e.baseId === 0x1397d)?.count ?? 0 });
};
const { RemoteServer } = require("../src/services/services/remoteServer") as { RemoteServer: typeof RemoteServerClass };

const service = () => {
  const controller: any = new Proxy({
    on: () => {},
    once: () => {},
    emitter: { on: () => {}, emit: () => {} },
    lookupListener: () => anything,
  }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  const s: any = new RemoteServer(platform as any, controller);
  applied.length = 0;
  updates = [];
  // one of the server's sends of a container's whole record
  const record = (id: number, arrows: number) => s.onUpdatePropertyMessage({
    message: { t: 0, idx: 1, refrId: id, propName: "inventory", data: { entries: [{ baseId: 0x1397d, count: arrows }] }, baseRecordType: "CONT" },
  });
  return { record };
};

test("a burst of one container's records within a tick lands once, as its newest record", () => {
  const { record } = service();
  for (let i = 1; i <= 12; ++i) {
    record(CHEST, 60 + i);
  }
  tick();
  assert.deepEqual([...applied], [{ id: CHEST, arrows: 72 }]);
});

test("two containers' records in one tick each land once", () => {
  const { record } = service();
  record(CHEST, 60);
  record(OTHER_CHEST, 5);
  record(CHEST, 61);
  tick();
  assert.deepEqual([...applied].sort((a, b) => a.id - b.id), [{ id: OTHER_CHEST, arrows: 5 }, { id: CHEST, arrows: 61 }].sort((a, b) => a.id - b.id));
});

test("a record after the tick's apply lands at the next tick", () => {
  const { record } = service();
  record(CHEST, 60);
  tick();
  record(CHEST, 20);
  tick();
  assert.deepEqual([...applied], [{ id: CHEST, arrows: 60 }, { id: CHEST, arrows: 20 }]);
});
