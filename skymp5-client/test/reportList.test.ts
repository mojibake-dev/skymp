// The report list (HostService.hostedActorRemoteIds, which the movement and the state reports send from). An actor
// this game's word names whose cell the engine says is not attached here (another worldspace than the player's, an
// exterior while the player stands in an interior) and that this game does not host is reported while it stands in the
// first square it stood in there: the door's far side, whose report carries its record through the door and which the
// server's follow takes moves in. Once it walks on to another square there, the server refuses every move of it (the
// follow vouches for a square its follower has loaded, and this game has none loaded there), so it is reported no more
// and its record stays where the server has it. In the player's own worldspace the list is as before, every square
// alike, since a crossing into another host's square passes on that report. The platform module is a stand-in loaded
// under its name, as in test/knockKept.test.ts; the engine reads (the player's place, each actor's cell, worldspace
// and position, the grid square of each cell) are this file's.
import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import type { HostService as HostServiceClass } from '../src/services/services/hostService';

const TAMRIEL = 0x3c;
// a walled city's own worldspace, entered through its gate's load door
const CITY = 0x1a26f;
// an exterior cell of Tamriel outside the city's gate, its grid square (4, -2)
const GATE_CELL = 0x95fc;
// the city worldspace's persistent cell, the parent a persistent resident reports there
const CITY_PERSISTENT = 0x1a270;
// an inn's interior
const INN = 0x165b7;
// two residents this game's word names
const WALKER = 0x1a6a4;
const STAYER = 0x1a69e;

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
const { HostService } = require('../src/services/services/hostService') as {
  HostService: typeof HostServiceClass;
};

type Place = { cell: number; interior: boolean; world: number | null; x: number; y: number };

const cellOf = (id: number, interior: boolean) => ({
  getFormID: () => id,
  isInterior: () => interior,
  isAttached: () => false,
});
const worldOf = (id: number | null) => (id === null ? null : { getFormID: () => id });

// the player outside the gate, or in the inn
const OUTSIDE: Place = { cell: GATE_CELL, interior: false, world: TAMRIEL, x: 19000, y: -7300 };
const IN_THE_INN: Place = { cell: INN, interior: true, world: null, x: 0, y: 0 };
// a resident at the gate's outer side, and two squares further along the road (no cell this game has loaded)
const AT_THE_GATE: Place = { cell: GATE_CELL, interior: false, world: TAMRIEL, x: 19262, y: -7201 };
const DOWN_THE_ROAD: Place = { cell: GATE_CELL, interior: false, world: TAMRIEL, x: 27500, y: -7201 };
// inside the city: the gate's inner side, and the next square up the street
const INSIDE_THE_GATE: Place = { cell: CITY_PERSISTENT, interior: false, world: CITY, x: 1500, y: -2600 };
const UP_THE_STREET: Place = { cell: CITY_PERSISTENT, interior: false, world: CITY, x: 1500, y: 1700 };

const service = (playerAt: Place, walkerAt: Place) => {
  const at = new Map<number, Place>([
    [0x14, playerAt],
    [WALKER, walkerAt],
    [STAYER, AT_THE_GATE],
  ]);
  const refAt = (id: number) => ({
    getFormID: () => id,
    getParentCell: () => cellOf(at.get(id)!.cell, at.get(id)!.interior),
    getWorldSpace: () => worldOf(at.get(id)!.world),
    getPositionX: () => at.get(id)!.x,
    getPositionY: () => at.get(id)!.y,
  });
  const refs = new Map(Array.from(at.keys(), (id) => [id, refAt(id)]));
  const grids = new Map([[GATE_CELL, { world: TAMRIEL, x: 4, y: -2 }]]);
  const sp: any = {
    settings: { 'skymp5-client': {} },
    storage: {},
    Game: {
      getPlayer: () => refs.get(0x14),
      getFormEx: (id: number) => refs.get(id) ?? null,
    },
    ObjectReference: { from: (f: unknown) => f ?? null },
    Actor: { from: (f: unknown) => f ?? null },
    cellGridOf: (cell: number) => grids.get(cell) ?? null,
    pollCellGrids: () => {},
  };
  const world = {
    playerCharacterFormIdx: 0,
    forms: [
      { refrId: 0xff000000, isHostedByOther: false },
      { refrId: WALKER, isHostedByOther: false },
      { refrId: STAYER, isHostedByOther: false },
    ],
  };
  const handlers: Record<string, Array<() => void>> = {};
  const controller: any = {
    on: (name: string, fn: () => void) => {
      (handlers[name] ??= []).push(fn);
    },
    emitter: { on: () => {}, emit: () => {} },
    lookupListener: () => ({ getWorldModel: () => world }),
  };
  const s = new HostService(sp, controller) as any;
  // the next tick: the player's place is read again
  const tick = () => (handlers['update'] ?? []).forEach((fn) => {
    try {
      fn();
    } catch {
      // the tick's other work reads engine state this file does not stand in for
    }
  });
  const walkerTo = (p: Place) => {
    at.set(WALKER, p);
    tick();
  };
  return { s, walkerTo };
};

test("an actor this game's word names, walked by its engine through a load door into a worldspace the player is not in, is reported in the first square it stands in there and not once it walks on to another", () => {
  const { s, walkerTo } = service(OUTSIDE, AT_THE_GATE);
  s.loaded.add(GATE_CELL);
  s.hosted.set(GATE_CELL, 3);
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER]);
  walkerTo(INSIDE_THE_GATE);
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER], 'the door\'s far side: the crossing\'s own report');
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER], 'still in that square');
  walkerTo(UP_THE_STREET);
  assert.deepEqual(s.hostedActorRemoteIds(), [STAYER], 'walked on: no square there is loaded here');
});

test("an exterior actor this game's word names, while the player stands in an interior and this game hosts none of its squares, is reported in the first square it stands in and not once it walks on to another", () => {
  const { s, walkerTo } = service(IN_THE_INN, AT_THE_GATE);
  s.loaded.add(INN);
  s.hosted.set(INN, 4);
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER]);
  walkerTo(DOWN_THE_ROAD);
  assert.deepEqual(s.hostedActorRemoteIds(), [STAYER]);
});

test("an actor left out walks back into a square this game has loaded and is reported again, the first square counted afresh when it leaves again", () => {
  const { s, walkerTo } = service(OUTSIDE, INSIDE_THE_GATE);
  s.loaded.add(GATE_CELL);
  s.hosted.set(GATE_CELL, 3);
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER]);
  walkerTo(UP_THE_STREET);
  assert.deepEqual(s.hostedActorRemoteIds(), [STAYER]);
  walkerTo(AT_THE_GATE);
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER]);
  walkerTo(UP_THE_STREET);
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER], 'its first square inside this time');
});

test("an actor this game's word names walking into a square of the player's worldspace this game has not loaded is still reported, as a crossing into another host's square passes on that report", () => {
  const { s, walkerTo } = service(OUTSIDE, AT_THE_GATE);
  s.loaded.add(GATE_CELL);
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER]);
  walkerTo(DOWN_THE_ROAD);
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER]);
});

test("an exterior actor in a square this game hosts is reported while the player stands in an interior, wherever it walks in that square", () => {
  const { s } = service(IN_THE_INN, AT_THE_GATE);
  s.loaded.add(INN);
  s.hosted.set(GATE_CELL, 3);
  assert.deepEqual(s.hostedActorRemoteIds(), [WALKER, STAYER]);
});
