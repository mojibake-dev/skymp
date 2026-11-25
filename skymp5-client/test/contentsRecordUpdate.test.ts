// A dead body's contents change on the engine that hosts it, through ChangeFormService's own path
// (src/services/services/changeFormService.ts: the engine's events as the service hears them, then the update's
// recordChangedBodies and onBodyAtRest). The engine's kill changes the body's contents as it starts to die, a send
// before the sender's first dead sample, so neither the prop stream nor the sender's settle has the body yet: a record
// taken then holds the dying life state and the fall's first pose, and a seat that lays its copy by it stands the body
// back up to fall again. That change waits for the rest's record; a change to a body lying still is recorded at once.
// The platform module is a stand-in loaded under its name, as in test/shoutClaimUpdate.test.ts; the engine reads
// around the record (the body, the serialize native, this seat's epoch, the fall's two other readings) are this
// file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { MsgType } from "../src/messages";
import type * as ChangeFormModule from "../src/services/services/changeFormService";

// a bandit this seat hosts, a game-file actor
const BANDIT = 0x39fe4;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});

const body = {
  getFormID: () => BANDIT,
  isDead: () => true,
  is3DLoaded: () => true,
  getPositionX: () => 3120,
  getPositionY: () => -1488,
  getPositionZ: () => 1024,
};
const platform = new Proxy(
  { storage: {}, Actor: { from: (f: unknown) => (f === body ? body : null) } },
  { get: (t, k) => (k in t ? (t as any)[k] : anything) },
);
const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { ChangeFormService, CORPSE_REST_MIN_MS } = require("../src/services/services/changeFormService") as typeof ChangeFormModule;

// the serialize native's answer for the body: a whole record with nothing left unmapped
const RECORD = JSON.stringify({ recordType: 1, flags: 0x800004, version: 9, bodyHex: "0102", fnv1a: 0x5eed, remap: [], remapTruncated: false, census: { dropped: 0 } });

const realNow = Date.now;
let clock = 1_000_000;

const service = () => {
  Date.now = () => clock;
  const serialized: number[] = [];
  const sent: Array<{ t: number; formId: number }> = [];
  const sp: any = new Proxy({
    Game: { getFormEx: (id: number) => (id === BANDIT ? body : null) },
    ObjectReference: { from: (f: unknown) => f ?? null },
    callNative: (_cls: string, fn: string, _self: unknown, target: typeof body) => {
      if (fn === "SerializeChangeForm") {
        serialized.push(target.getFormID());
        return RECORD;
      }
      return "";
    },
  }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  // the fall's other two readings: the prop stream holds no hand or open stream on the body, and the sender has taken
  // no dead sample of it yet (its first comes a send after the kill)
  const props = new Proxy({ bodyFalling: () => false, followsBody: () => false }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  const host = new Proxy({ epochForActor: () => 7, isMineLocal: () => true }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  // every listener of an event, as the controller's own emitter calls them
  const listeners: Record<string, Array<(e?: unknown) => void>> = {};
  const controller: any = {
    on: (name: string, fn: (e?: unknown) => void) => { (listeners[name] ??= []).push(fn); },
    once: () => {},
    emitter: {
      on: () => {},
      emit: (name: string, e: { message: { t: number; formId: number } }) => {
        if (name === "sendMessage") {
          sent.push(e.message);
        }
      },
    },
    lookupListener: (cls: { name: string }) => (cls.name === "PropService" ? props : cls.name === "HostService" ? host : anything),
  };
  new ChangeFormService(sp, controller);
  const raise = (name: string, e?: unknown) => (listeners[name] ?? []).forEach((fn) => fn(e));
  const records = () => sent.filter((m) => m.t === MsgType.ChangeForm && m.formId === BANDIT).length;
  return { raise, serialized, records };
};

test("the engine's kill of a body this seat hosts changes its contents before the first dead sample: no record is taken then", () => {
  const { raise, serialized, records } = service();
  raise("deathStart", { actorDying: body, actorKiller: null });
  raise("containerChanged", { oldContainer: null, newContainer: body });
  raise("update");
  assert.deepEqual(serialized, [], "the dying body is not serialized");
  assert.equal(records(), 0, "no record of the dying body is sent");
  // still inside the rest's floor: the rest's record, CORPSE_REST_MIN_MS or more after the kill, carries the contents
  clock += CORPSE_REST_MIN_MS - 1;
  raise("containerChanged", { oldContainer: body, newContainer: null });
  raise("update");
  assert.equal(records(), 0, "a change inside the floor waits for the rest's record too");
  Date.now = realNow;
});

test("a take from the body once it has lain dead past the rest's floor is recorded at once", () => {
  const { raise, serialized, records } = service();
  raise("deathStart", { actorDying: body, actorKiller: null });
  clock += CORPSE_REST_MIN_MS;
  raise("containerChanged", { oldContainer: body, newContainer: null });
  raise("update");
  assert.deepEqual(serialized, [BANDIT]);
  assert.equal(records(), 1);
  Date.now = realNow;
});

test("a take from a body that died before this seat saw it is recorded at once", () => {
  const { raise, serialized, records } = service();
  raise("containerChanged", { oldContainer: body, newContainer: null });
  raise("update");
  assert.deepEqual(serialized, [BANDIT]);
  assert.equal(records(), 1);
  Date.now = realNow;
});
