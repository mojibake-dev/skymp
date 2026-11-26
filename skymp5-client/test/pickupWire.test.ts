// A pickup this game's engine ran is reported as one take (ContainersService's pickUpObject handler,
// src/services/services/containersService.ts), matched against the player's inventory difference in the wire's shape,
// the record's own: a created potion there is its template and its definition (sync/createdForms.ts toWireEntry),
// never the engine's session id, which no other game knows. The platform module is a stand-in loaded under its name,
// as in test/shoutClaimUpdate.test.ts; its created-form natives answer for one brewed potion.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type * as ContainersModule from "../src/services/services/containersService";
import type * as LastInvModule from "../src/services/services/lastInvService";
import type * as ActivationModule from "../src/services/services/activationService";

// a brewed potion as this engine holds it (a session form), its definition, and the template the wire names it by
const SESSION_POTION = 0xff000a12;
const TEMPLATE = 0x3eb15;
const DEFINITION = { kind: 2, effects: [{ mgef: 0x3eb15, magnitude: 25, area: 0, duration: 0, cost: 0 }] };
const POTION_REF = 0xff000b40;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});

// the player's inventory changes after the pickup: one brewed potion gained
const playerChanges = [{ baseId: SESSION_POTION, countDelta: 1, extendDataList: [] }];
const player = { getFormID: () => 0x14, getBaseObject: () => ({ getFormID: () => 0x7 }) };

const platform = new Proxy(
  {
    storage: {},
    on: () => undefined,
    once: () => undefined,
    getContainer: () => [],
    getExtraContainerChanges: (id: number) => (id === 0x14 ? playerChanges : []),
    readCreatedForm: (id: number) => (id === SESSION_POTION ? DEFINITION : null),
    createdFormTemplate: (kind: number) => (kind === 2 ? TEMPLATE : 0),
    createCreatedForm: () => 0,
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
const { ContainersService } = require("../src/services/services/containersService") as typeof ContainersModule;
const { LastInvService } = require("../src/services/services/lastInvService") as typeof LastInvModule;
const { ActivationService } = require("../src/services/services/activationService") as typeof ActivationModule;

test("a brewed potion this game picks up is reported in the wire's shape, its template and definition, and the baseline gains no session id", () => {
  const sent: any[] = [];
  const handlers: Record<string, (e: unknown) => void> = {};
  const lastInv = { lastInv: { entries: [] as any[] } };
  const controller: any = {
    on: (name: string, fn: (e: unknown) => void) => { handlers[name] = fn; },
    once: () => undefined,
    emitter: {
      on: () => undefined,
      emit: (name: string, e: { message: unknown }) => { if (name === "sendMessage") sent.push(e.message); },
    },
    lookupListener: (cls: unknown) => (cls === LastInvService ? lastInv : anything),
  };
  const sp: any = new Proxy(
    { Game: { getPlayer: () => player, getFormEx: () => ({ getName: () => "Potion" }) } },
    { get: (t, k) => (k in t ? (t as any)[k] : anything) },
  );
  new ContainersService(sp, controller);
  handlers["pickUpObject"]({ refId: POTION_REF, baseId: SESSION_POTION, count: 1, ownerId: 0 });

  assert.equal(sent.length, 1);
  assert.equal(sent[0].baseId, TEMPLATE);
  assert.deepEqual(sent[0].createdBase, DEFINITION);
  assert.equal(sent[0].count, 1);
  assert.deepEqual(lastInv.lastInv.entries.filter((e: any) => e.baseId > 0xfeffffff), []);
});

test("this player's own activation takes the container diff's baseline in the wire's shape, a brewed potion it holds by its template and definition", () => {
  const handlers: Record<string, (e: unknown) => void> = {};
  const lastInv = { lastInv: undefined as any };
  const controller: any = new Proxy({
    on: (name: string, fn: (e: unknown) => void) => { handlers[name] = fn; },
    once: () => undefined,
    emitter: { on: () => undefined, emit: () => undefined },
    lookupListener: (cls: unknown) => (cls === LastInvService ? lastInv : anything),
  }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  const sp: any = new Proxy(
    { Game: { getPlayer: () => player, getFormEx: () => ({ getName: () => "Potion" }) } },
    { get: (t, k) => (k in t ? (t as any)[k] : anything) },
  );
  new ActivationService(sp, controller);
  handlers["activate"]({ caster: player, target: null });

  assert.deepEqual(lastInv.lastInv.entries.filter((e: any) => e.baseId > 0xfeffffff), []);
  const potion = lastInv.lastInv.entries.find((e: any) => e.baseId === TEMPLATE);
  assert.ok(potion, JSON.stringify(lastInv.lastInv));
  assert.deepEqual(potion.createdBase, DEFINITION);
});
