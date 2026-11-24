// The roster (HostService.reportSightings): the game-file actors in this engine's high-process list that the server gave
// this game no word on, with where the engine has them, every sightingsMs. The server takes a sighting in a cell this
// game hosts as that cell's record, and tells a sighting in a cell another game hosts to move this game's own copy to
// the record (PartOne::OnActorSighting). A game that hosts nothing sends it too: its engine loads the actors around its
// player and places each by its own schedule, so a resident whose record another seat's engine walked through a city's
// gate stands outside on this game, wordless and frozen, until the server's answer moves the copy to its record. The
// platform module is a stand-in loaded under its name, as in test/reportList.test.ts.
import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import { MsgType } from '../src/messages';
import type { HostService as HostServiceClass } from '../src/services/services/hostService';

const TAMRIEL = 0x3c;
const GATE_CELL = 0x95fc;
// a resident whose copy this engine placed outside the city's gate, with no word from the server; one it has a word on
const WORDLESS = 0x1a6a4;
const WORDED = 0x1a69e;

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

const service = () => {
  const ref = (id: number, x: number) => ({
    getFormID: () => id,
    isDisabled: () => false,
    getWorldSpace: () => ({ getFormID: () => TAMRIEL }),
    getParentCell: () => ({ getFormID: () => GATE_CELL, isInterior: () => false }),
    getPositionX: () => x,
    getPositionY: () => -7201,
    getPositionZ: () => -3595,
    getAngleX: () => 0,
    getAngleY: () => 0,
    getAngleZ: () => 90,
  });
  const refs = new Map([
    [WORDLESS, ref(WORDLESS, 19262)],
    [WORDED, ref(WORDED, 19500)],
  ]);
  const sent: any[] = [];
  const sp: any = {
    settings: { 'skymp5-client': {} },
    storage: {},
    Game: { getFormEx: (id: number) => refs.get(id) ?? null, getPlayer: () => null },
    ObjectReference: { from: (f: unknown) => f ?? null },
    Actor: { from: (f: unknown) => f ?? null },
    getHighProcessActors: () => [WORDLESS, WORDED, 0x14],
  };
  const world = {
    playerCharacterFormIdx: 0,
    forms: [{ refrId: 0xff000001, isHostedByOther: false }, { refrId: WORDED, isHostedByOther: true }],
  };
  const controller: any = {
    on: () => {},
    emitter: {
      on: () => {},
      emit: (name: string, e: unknown) => {
        if (name === 'sendMessage') {
          sent.push(e);
        }
      },
    },
    lookupListener: () => ({ getWorldModel: () => world }),
  };
  return { s: new HostService(sp, controller) as any, sent };
};

test("a game that hosts nothing names its wordless copies in its roster, with where its engine has each", () => {
  const { s, sent } = service();
  assert.equal(s.hostedCells().size, 0);
  s.reportSightings(Date.now());
  assert.equal(sent.length, 1);
  assert.equal(sent[0].reliability, 'reliable');
  assert.equal(sent[0].message.t, MsgType.ActorSightings);
  assert.deepEqual(sent[0].message.sightings, [
    { refrId: WORDLESS, worldOrCell: TAMRIEL, pos: [19262, -7201, -3595], rot: [0, 0, 90] },
  ]);
});
