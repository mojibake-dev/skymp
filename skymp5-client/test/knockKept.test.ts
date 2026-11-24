// A body this seat's engine knocks past TRACK_RADIUS, kept by the rescan until its stream's first sample, through
// PropService's own update path (src/services/services/propService.ts: the relayed push's handler, onUpdate,
// trackKnocks, watchForced, rowOrRefusal, rescan, keptAnywhere and untrack). Its host owns the body already, so the
// row carries no claim; a rescan that falls between the knock's track and the stream's first sample finds the body
// past TRACK_RADIUS and untracks it as out of reach, and the throw streams to no other seat. The platform module is a
// stand-in loaded under its name, as in test/shoutClaimUpdate.test.ts; the engine reads (the body's class and place,
// its kind) are this file's, and the ticks are too, so the stream's first sample is never sent.
import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import type { PropService as PropServiceClass } from '../src/services/services/propService';
import { KNOCK_KEEP_MS, knockKept } from '../src/sync/knockKept';

const PLAYER = 0x14;
// a draugr this seat hosts, a game-file actor (its local id is its server id)
const DRAUGR = 0x380b9;
// its place, 1496 units from the player, past TRACK_RADIUS (1024)
const FAR = 1496;

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
// the world view the rescan reads: one form view, the draugr's
const forms = [{ refrId: DRAUGR, baseId: 0x567cb, isHostedByOther: false }];
const storage: Record<string, unknown> = {
  view: {
    getRemoteRefrId: () => 0,
    getLocalRefrId: () => 0,
    getFormViews: () => ({
      getFormViewsArrayLength: () => forms.length,
      getNthFormView: (i: number) => ({
        getLocalRefrId: () => forms[i].refrId,
      }),
    }),
  },
};
const platform = new Proxy(
  { storage },
  { get: (t, k) => (k in t ? (t as any)[k] : anything) },
);

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
const { PropService } = require('../src/services/services/propService') as {
  PropService: typeof PropServiceClass;
};

const cell = { getFormID: () => 0x371de };
const draugr = {
  getBaseObject: () => ({ getFormID: () => 0x567cb }),
  getFormID: () => DRAUGR,
  isDead: () => false,
  getPositionX: () => FAR,
  getPositionY: () => 0,
  getPositionZ: () => 0,
  getWorldSpace: () => null,
  getParentCell: () => cell,
  is3DLoaded: () => true,
  setMotionType: () => Promise.resolve(),
};
const player = {
  getFormID: () => PLAYER,
  getPositionX: () => 0,
  getPositionY: () => 0,
  getPositionZ: () => 0,
  getWorldSpace: () => null,
  getParentCell: () => cell,
};
const sp: any = new Proxy(
  {
    Actor: { from: (f: unknown) => (f === draugr ? f : null) },
    ObjectReference: { from: (f: unknown) => f ?? null },
    Game: {
      getFormEx: (id: number) => (id === DRAUGR ? draugr : null),
      getPlayer: () => player,
    },
  },
  { get: (t, k) => (k in t ? (t as any)[k] : anything) },
);

const service = () => {
  const handlers: Record<string, (e?: unknown) => void> = {};
  const messages: Record<string, (e?: unknown) => void> = {};
  const worldModel = { playerCharacterFormIdx: -1, forms };
  const controller: any = {
    on: (name: string, fn: (e?: unknown) => void) => {
      handlers[name] = fn;
    },
    emitter: {
      on: (name: string, fn: (e?: unknown) => void) => {
        messages[name] = fn;
      },
      emit: () => {},
    },
    lookupListener: () =>
      new Proxy(
        {
          getWorldModel: () => worldModel,
          isMineLocal: (id: number) => id === DRAUGR,
        },
        { get: (t, k) => (k in t ? (t as any)[k] : anything) },
      ),
  };
  const s: any = new PropService(sp, controller);
  // the update's steps that read the engine for other bodies, and the ticks that would send the stream's first sample
  for (const step of [
    'applyImpulseRule',
    'applyWorldBodyContacts',
    'sweepEngineRefs',
    'readHolds',
    'feed',
    'claimIslands',
    'tick',
    'sendDue',
  ]) {
    s[step] = () => {};
  }
  s.readSimulated = () => new Map();
  s.enabled = () => true;
  s.myId = () => 7;
  // the engine reads: the draugr's body ragdolled, a world body of its own, placed where its engine has it
  s.bodyClass = (localId: number) =>
    localId === DRAUGR
      ? { ragdoll: true, parts: [0], bones: [], cut: false }
      : null;
  s.kindOf = (localId: number) => (localId === DRAUGR ? 'world' : 'none');
  s.placeInPlace = (refr: number, localId: number) => {
    const prop = (PropService as any).newProp(
      refr,
      localId,
      -1,
      cell.getFormID(),
      FAR,
      0,
      true,
      18,
    );
    prop.placed = true;
    prop.loaded3D = true;
    prop.live = true;
    s.props.set(refr, prop);
    s.byLocal.set(localId, refr);
    return prop;
  };
  // the next update runs the rescan, as the next RESCAN_MS does
  const rescanDue = () => {
    s.lastRescan = -Infinity;
  };
  return { handlers, messages, s, rescanDue };
};

// the snippet OnPushActor sends the host: ObjectReference.PushActorAway on the pusher, the target a game-file actor by
// its long id (SpSnippet::MakeLongFormId)
const relayedPush = (messages: Record<string, (e?: unknown) => void>) =>
  messages.spSnippetMessage({
    message: {
      class: 'ObjectReference',
      function: 'PushActorAway',
      selfId: 0xff000001,
      arguments: [{ formId: DRAUGR + 0x100000000, type: 'Actor' }, 15],
      snippetIdx: 1,
    },
  });

test('a body knocked moments ago is kept; one no knock tracked, or knocked longer ago than the window, is not', () => {
  assert.equal(knockKept(1000, 1076), true);
  assert.equal(knockKept(1000, 1000 + KNOCK_KEEP_MS - 1), true);
  assert.equal(knockKept(1000, 1000 + KNOCK_KEEP_MS), false);
  assert.equal(knockKept(0, 1076), false);
});

test("the server's relayed push onto a draugr this seat hosts, past TRACK_RADIUS, stays tracked through the rescan before its stream's first sample", () => {
  const { handlers, messages, s, rescanDue } = service();
  relayedPush(messages);
  handlers.update();
  assert.ok(s.props.has(DRAUGR), "the draugr's knock makes it a row");
  rescanDue();
  handlers.update();
  assert.ok(
    s.props.has(DRAUGR),
    'the rescan keeps the knocked draugr until its stream starts',
  );
});

test("this seat's own shout at a draugr it hosts, past TRACK_RADIUS, stays tracked through the rescan before its stream's first sample", () => {
  const { handlers, s, rescanDue } = service();
  // the shout's effect applied by this seat's player on the draugr (a creature's graph queues no knock edge)
  handlers.magicEffectApply({
    caster: { getFormID: () => PLAYER },
    target: { getFormID: () => DRAUGR },
  });
  handlers.update();
  assert.ok(s.props.has(DRAUGR), "the draugr's knock makes it a row");
  rescanDue();
  handlers.update();
  assert.ok(
    s.props.has(DRAUGR),
    'the rescan keeps the knocked draugr until its stream starts',
  );
});

test("a knock edge of an NPC this seat hosts, past TRACK_RADIUS, stays tracked through the rescan before its stream's first sample", () => {
  const { handlers, s, rescanDue } = service();
  // the action hook's knock edge for a hosted NPC
  s.knocks.push(DRAUGR);
  handlers.update();
  assert.ok(s.props.has(DRAUGR), 'the knock edge makes it a row');
  rescanDue();
  handlers.update();
  assert.ok(
    s.props.has(DRAUGR),
    'the rescan keeps the knocked NPC until its stream starts',
  );
});

test('a knocked draugr whose stream never starts within the window is untracked by the rescan after it', () => {
  const { handlers, messages, s, rescanDue } = service();
  relayedPush(messages);
  handlers.update();
  const row = s.props.get(DRAUGR);
  assert.ok(row, "the draugr's knock makes it a row");
  // the window over (the test's clock starts near 0)
  row.knockTrackedAt = -KNOCK_KEEP_MS - 1;
  rescanDue();
  handlers.update();
  assert.equal(s.props.has(DRAUGR), false);
});
