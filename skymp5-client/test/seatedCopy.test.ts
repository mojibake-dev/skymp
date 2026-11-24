// A copy the game file ties to a furniture (its linked reference: Arvel's web marker, a ceiling spider's ambush
// furniture) is its engine's to seat at the load, and a copy frozen before that seat completes never sits: with its AI
// off the engine does not seat it, and the promote's package evaluation does not either. So a copy standing near its
// linked furniture but not seated in it is not frozen at its first 3D until its engine seats it or SEAT_WAIT_MS pass;
// a seated copy frozen keeps its pose. The pure rules are src/sync/seatWait.ts; the demote's own path runs here,
// through PuppetService.reconcile, with the platform module a stand-in loaded under its name, as in
// test/knockKept.test.ts.
import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import { SEAT_WAIT_MS, seatVerdict, seated } from '../src/sync/seatWait';
import type { PuppetService as PuppetServiceClass } from '../src/services/services/puppetService';

// Arvel, his web marker (WebFurnitureMarker 0x45d99), and a resident tied to no furniture
const ARVEL = 0x39655;
const WEB = 0x46e90;
const RESIDENT = 0x1a69e;
const WEB_AT = { x: 3917.94, y: -3313.38, z: -1778.89 };

test("a copy is seated at sit state 3, or held in its linked furniture within a few units of it (a ceiling spider reads sit state 0 there)", () => {
  // seated: Arvel at sit state 3, the spider in its ceiling furniture 0.3 units off it
  assert.equal(seated({ sitState: 3, furnitureIsLinked: true, unitsFromFurniture: 0.4 }), true);
  assert.equal(seated({ sitState: 0, furnitureIsLinked: true, unitsFromFurniture: 0.3 }), true);
  // not seated: Arvel 13 units under his seat with no furniture yet, the spider 7 units off its ceiling
  assert.equal(seated({ sitState: 0, furnitureIsLinked: false, unitsFromFurniture: 13 }), false);
  assert.equal(seated({ sitState: 0, furnitureIsLinked: true, unitsFromFurniture: 7 }), false);
});

test('a copy near its linked furniture and not seated waits SEAT_WAIT_MS for its seat; a seated one, one far from it, or one tied to none is frozen at once', () => {
  const near = { linkedFurniture: true, seated: false, unitsFromFurniture: 13, sinceMs: 0 };
  assert.equal(seatVerdict(near), 'wait');
  assert.equal(seatVerdict({ ...near, sinceMs: SEAT_WAIT_MS - 1 }), 'wait');
  assert.equal(seatVerdict({ ...near, sinceMs: SEAT_WAIT_MS }), 'freeze');
  assert.equal(seatVerdict({ ...near, seated: true }), 'freeze');
  assert.equal(seatVerdict({ ...near, unitsFromFurniture: 300 }), 'freeze');
  assert.equal(seatVerdict({ ...near, linkedFurniture: false }), 'freeze');
});

const anything: any = new Proxy(
  function () {
    return anything;
  },
  {
    get: (_t, k) =>
      k === 'then' ? undefined : k === Symbol.toPrimitive ? () => 0 : anything,
    apply: () => anything,
    construct: () => anything,
  },
);
const platform = new Proxy({}, { get: () => anything });
const Module = require('module');
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ['skyrimPlatform', '@skyrim-platform/skyrim-platform'];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request)
    ? 'skyrimPlatform'
    : resolve.call(this, request, ...rest);
};
require.cache['skyrimPlatform'] = {
  id: 'skyrimPlatform',
  filename: 'skyrimPlatform',
  loaded: true,
  exports: platform,
} as any;
const { PuppetService } = require('../src/services/services/puppetService') as {
  PuppetService: typeof PuppetServiceClass;
};

const service = () => {
  // Arvel's engine state at a load whose freeze came before his seat: 13 units under it, no furniture yet
  const arvel = { z: -1792, sit: 0, furniture: 0 };
  const web = {
    getFormID: () => WEB,
    getBaseObject: () => ({ getFormID: () => 0x45d99 }),
    getPositionX: () => WEB_AT.x,
    getPositionY: () => WEB_AT.y,
    getPositionZ: () => WEB_AT.z,
  };
  const actor = (id: number) => ({
    getFormID: () => id,
    getBaseObject: () => ({ getFormID: () => (id === ARVEL ? 0x39646 : 0x1a69d) }),
    is3DLoaded: () => true,
    isDead: () => false,
    getPositionX: () => (id === ARVEL ? 3918 : 5000),
    getPositionY: () => (id === ARVEL ? -3313 : 5000),
    getPositionZ: () => (id === ARVEL ? arvel.z : 0),
    getSitState: () => (id === ARVEL ? arvel.sit : 0),
    getFurnitureReference: () => (id === ARVEL && arvel.furniture === WEB ? web : null),
    getLinkedRef: () => (id === ARVEL ? web : null),
    getCurrentScene: () => null,
    isInCombat: () => false,
    getCombatTarget: () => null,
    getWorldSpace: () => null,
    getParentCell: () => ({ getFormID: () => 0x371de, isInterior: () => true, isAttached: () => true }),
  });
  // the reads that matter here are the actor's own; every other engine call reconcile makes is a no-op
  const withRest = (o: object) => new Proxy(o, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  const actors = new Map([
    [ARVEL, withRest(actor(ARVEL))],
    [RESIDENT, withRest(actor(RESIDENT))],
  ]);
  const puppetCalls: Array<[number, boolean]> = [];
  const sp: any = new Proxy(
    {
      settings: { 'skymp5-client': {} },
      storage: {},
      Game: { getFormEx: (id: number) => actors.get(id) ?? (id === WEB ? web : null), getPlayer: () => null },
      ObjectReference: { from: (f: unknown) => f ?? null },
      Actor: { from: (f: any) => (f && f.getSitState ? f : null) },
      Furniture: { from: (f: any) => (f && f.getFormID && f.getFormID() === 0x45d99 ? f : null) },
      isPuppet: (id: number) => puppetCalls.some(([p, on]) => p === id && on),
      setPuppet: (id: number, on: boolean) => {
        puppetCalls.push([id, on]);
        return true;
      },
      getPackageDestinationCells: () => [],
    },
    { get: (t, k) => (k in t ? (t as any)[k] : anything) },
  );
  const host = new Proxy(
    {
      isMineLocal: () => false,
      isOthersLocal: () => false,
      cellAttachedFor: () => undefined,
      hostsCell: () => false,
      markHosted: () => {},
    },
    { get: (t, k) => (k in t ? (t as any)[k] : anything) },
  );
  const props = new Proxy({ bodyOwnedElsewhere: () => false }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  const handlers: Record<string, () => void> = {};
  const controller: any = {
    on: (name: string, fn: () => void) => {
      handlers[name] = fn;
    },
    emitter: { on: () => {}, emit: () => {} },
    lookupListener: (cls: { name: string }) => (cls.name === 'PropService' ? props : cls.name === 'HostService' ? host : anything),
  };
  const s = new PuppetService(sp, controller) as any;
  return { s, arvel, puppetCalls, handlers };
};

test("Arvel's copy, under his web and not yet seated at its first 3D, is not frozen until his engine seats him, then frozen seated", () => {
  const { s, arvel, puppetCalls } = service();
  s.reconcile(ARVEL, false);
  assert.deepEqual(puppetCalls, [], 'not frozen while his engine seats him');
  arvel.z = WEB_AT.z;
  arvel.sit = 3;
  arvel.furniture = WEB;
  s.reconcile(ARVEL, false);
  assert.deepEqual(puppetCalls, [[ARVEL, true]], 'frozen once seated');
});

test('a copy tied to no furniture is frozen at its first 3D as before', () => {
  const { s, puppetCalls } = service();
  s.reconcile(RESIDENT, false);
  assert.deepEqual(puppetCalls, [[RESIDENT, true]]);
});

test('a copy its engine does not seat within SEAT_WAIT_MS is frozen where it stands', () => {
  const { s, puppetCalls } = service();
  s.reconcile(ARVEL, false);
  assert.deepEqual(puppetCalls, []);
  s.seatWaitSince.set(ARVEL, Date.now() - SEAT_WAIT_MS);
  s.reconcile(ARVEL, false);
  assert.deepEqual(puppetCalls, [[ARVEL, true]]);
});


test("a copy waiting for its seat is frozen at the bound by the update, though nothing reconciles it again", () => {
  const { s, puppetCalls, handlers } = service();
  s.reconcile(ARVEL, false);
  assert.deepEqual(puppetCalls, []);
  s.seatWaitSince.set(ARVEL, Date.now() - SEAT_WAIT_MS);
  handlers.update();
  assert.deepEqual(puppetCalls, [[ARVEL, true]]);
});
