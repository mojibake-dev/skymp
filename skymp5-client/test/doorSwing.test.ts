// A door's activation while the door still swings, through ActivationService's own handler
// (src/services/services/activationService.ts, the activate event). An NPC this game hosts walks its schedule to a load
// door right behind another walker, and the engine's activation comes while the door is still opening or closing from
// the first crossing. Every bound door carries the block, so the engine never crosses him itself: his crossing is his
// host's report (the server's door walk answers it), and dropping it for the swing leaves him at the door for good. A
// player's own press mid-swing, and a plain door's toggle, are dropped as before. The platform module is a stand-in
// loaded under its name, as in test/shoutClaimUpdate.test.ts; the engine reads around the event are this file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { MsgType } from "../src/messages";
import type { ActivationService as ActivationServiceClass } from "../src/services/services/activationService";

// a resident this game hosts, the inn's street door and its far side, and a plain door in the same street
const RESIDENT = 0x1a68a;
const INN_DOOR = 0x1a6f4;
const INN_DOOR_FAR = 0x1a6f5;
const PLAIN_DOOR = 0x1a700;
const PLAYER = 0x14;
const DOOR_TYPE = 29;
// Skyrim Platform's open states: 0 none, 1 open, 2 opening, 3 closed, 4 closing
const OPENING = 2;
const CLOSING = 4;
const CLOSED = 3;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
const platform = new Proxy({ storage: {} }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { ActivationService } = require("../src/services/services/activationService") as { ActivationService: typeof ActivationServiceClass };
const { SpApiInteractor } = require("../src/services/spApiInteractor");

// the world model names the resident, a game-file actor the server sent (isSyncedActorId)
(SpApiInteractor as any).controller = {
  lookupListener: () => ({ getWorldModel: () => ({ forms: [{ refrId: RESIDENT }] }) }),
};

const service = (doors: Record<number, number | null>) => {
  const resident = { getFormID: () => RESIDENT, isDead: () => false };
  const door = (id: number, openState: number) => ({
    getFormID: () => id,
    getBaseObject: () => ({ getType: () => DOOR_TYPE, getFormID: () => 0x1a6f0 }),
    isActivationBlocked: () => true,
    getOpenState: () => openState,
  });
  const polled: number[] = [];
  const sp: any = new Proxy({
    settings: { "skymp5-client": {} },
    FormType: { Door: DOOR_TYPE },
    Actor: { from: (f: unknown) => (f === resident ? resident : null) },
    Game: { getPlayer: () => anything, getFormEx: () => null },
    isPuppet: () => false,
    setTriggerLook: () => true,
    linkedDoorOf: (id: number) => (id in doors ? doors[id] : null),
    pollLinkedDoors: (ids: number[]) => { polled.push(...ids); },
  }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  const host = new Proxy(
    { isMineLocal: (id: number) => id === RESIDENT, isOthersLocal: () => false, isHostedHere: () => true, hostsRef: () => true },
    { get: (t, k) => (k in t ? (t as any)[k] : anything) },
  );
  const listeners: Record<string, Array<(e?: unknown) => void>> = {};
  const sent: Array<{ t: number; data: { caster: number; target: number } }> = [];
  const controller: any = {
    on: (name: string, fn: (e?: unknown) => void) => { (listeners[name] ??= []).push(fn); },
    once: () => {},
    emitter: {
      on: () => {},
      emit: (name: string, e: { message: { t: number; data: { caster: number; target: number } } }) => {
        if (name === "sendMessage") {
          sent.push(e.message);
        }
      },
    },
    lookupListener: (cls: { name: string }) => (cls.name === "HostService" ? host : anything),
  };
  new ActivationService(sp, controller);
  const activate = (casterId: number, target: { getFormID: () => number }) => {
    const caster = casterId === RESIDENT ? resident : { getFormID: () => casterId, isDead: () => false };
    (listeners["activate"] ?? []).forEach((fn) => fn({ caster, target }));
  };
  const activations = () => sent.filter((m) => m.t === MsgType.Activate).map((m) => [m.data.caster, m.data.target]);
  return { activate, activations, polled, door };
};

test("a hosted resident at a load door that still swings from the walker ahead of him: his crossing is reported", () => {
  const { activate, activations, door } = service({ [INN_DOOR]: INN_DOOR_FAR });
  activate(RESIDENT, door(INN_DOOR, OPENING));
  activate(RESIDENT, door(INN_DOOR, CLOSING));
  assert.deepEqual(activations(), [[RESIDENT, INN_DOOR], [RESIDENT, INN_DOOR]]);
});

test("a hosted resident at a load door this seat has not read yet: the door's link is asked, and his next try reports", () => {
  const doors: Record<number, number | null> = {};
  const { activate, activations, polled, door } = service(doors);
  activate(RESIDENT, door(INN_DOOR, CLOSING));
  assert.deepEqual(activations(), []);
  assert.deepEqual(polled, [INN_DOOR]);
  doors[INN_DOOR] = INN_DOOR_FAR;
  activate(RESIDENT, door(INN_DOOR, CLOSING));
  assert.deepEqual(activations(), [[RESIDENT, INN_DOOR]]);
});

test("a hosted resident at a plain door that still swings is dropped as before", () => {
  const { activate, activations, door } = service({ [PLAIN_DOOR]: 0 });
  activate(RESIDENT, door(PLAIN_DOOR, OPENING));
  assert.deepEqual(activations(), []);
});

test("the player's own press on a door that still swings is dropped as before, and a still door's is sent", () => {
  const { activate, activations, door } = service({ [INN_DOOR]: INN_DOOR_FAR });
  activate(PLAYER, door(INN_DOOR, OPENING));
  assert.deepEqual(activations(), []);
  activate(PLAYER, door(INN_DOOR, CLOSED));
  assert.equal(activations().length, 1);
});
