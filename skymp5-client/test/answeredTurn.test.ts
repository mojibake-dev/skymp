// A pillar turn the host's own script made while the pillar's 3D was away, through the services' own paths
// (src/services/services/scriptCallService.ts: the scriptCalls handler hands an answered action to
// ObjectAnimationService; objectAnimationService.ts: answeredAtOnce keeps the play and the 3D poll shows it on the
// host's graph). The platform module is a stand-in loaded under its name. Its natives answer only inside an update,
// as Skyrim Platform's do (a native called from the scriptCalls event throws, CallNativeApi.cpp); the pillar's
// reads (its 3D, its graph's plays) are this file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { ScriptCallService as ScriptCallServiceClass } from "../src/services/services/scriptCallService";
import type { ObjectAnimationService as ObjectAnimationServiceClass } from "../src/services/services/objectAnimationService";

// Bleak Falls Temple's first puzzle pillar (base 0xaa8ce) and its cell
const PILLAR = 0xaa8cb;
const TEMPLE = 0x371de;

// every property of the stand-in a function that returns the stand-in, so module-level reads load and a call made
// through it does nothing
const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});

// whether the engine runs natives now: inside an update alone
let inUpdate = false;
const native = <A extends unknown[], R>(fn: (...a: A) => R) => (...a: A): R => {
  if (!inUpdate) {
    throw new Error("can't be called in this context");
  }
  return fn(...a);
};

let loaded = false;
const plays: string[] = [];
const pillar = {
  getFormID: native(() => PILLAR),
  is3DLoaded: native(() => loaded),
  playAnimation: native((event: string) => {
    plays.push(event);
    return true;
  }),
  getParentCell: native(() => ({ getFormID: () => TEMPLE })),
  getBaseObject: native(() => ({ getFormID: () => 0xaa8ce, getType: () => 0 })),
};

const platform = new Proxy({
  storage: {},
  ObjectReference: { from: (f: unknown) => (f === pillar ? pillar : null) },
  FormType: { Furniture: -1 },
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

const Module = require("module");
const resolve = Module._resolveFilename;
// the platform under both names the client's modules import it by
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { ScriptCallService } = require("../src/services/services/scriptCallService") as { ScriptCallService: typeof ScriptCallServiceClass };
const { ObjectAnimationService } = require("../src/services/services/objectAnimationService") as { ObjectAnimationService: typeof ObjectAnimationServiceClass };

const sp: any = new Proxy({
  settings: { "skymp5-client": {} },
  Actor: { from: native(() => null) },
  Game: { getFormEx: native((id: number) => (id === PILLAR ? pillar : null)) },
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

// this seat hosts the temple; no record of the pillar is being applied
const others: Record<string, unknown> = {
  ScriptStateService: { noteCall: () => {}, viewOf: () => undefined, afterHosting: () => {} },
  SpSnippetService: { seqOf: () => undefined },
  HostService: { epochForActor: () => 3, ownerFlips: () => 0, hostsCell: () => true },
};

const seat = () => {
  const handlers: Record<string, Array<(e?: unknown) => void>> = {};
  const onces: Record<string, Array<(e?: unknown) => void>> = {};
  const listeners: Record<string, unknown> = { ...others };
  const controller: any = {
    on: (name: string, fn: (e?: unknown) => void) => { (handlers[name] ??= []).push(fn); },
    once: (name: string, fn: (e?: unknown) => void) => { (onces[name] ??= []).push(fn); },
    emitter: { on: () => {}, emit: () => {} },
    lookupListener: (cls: { name: string }) => listeners[cls.name],
  };
  const animation: any = new ObjectAnimationService(sp, controller);
  const calls: any = new ScriptCallService(sp, controller);
  listeners.ObjectAnimationService = animation;
  listeners.ScriptCallService = calls;
  // the updates' other work, which reads the engine for other objects and sends nothing here
  animation.onUpdate = () => {};
  animation.retryHeldPlays = () => {};
  calls.onUpdate = () => {};
  // the platform raises its events in turn; a handler's throw is the platform's to log, and the event goes on
  const raise = (name: string, e?: unknown) => {
    const due = onces[name] ?? [];
    onces[name] = [];
    for (const fn of [...due, ...(handlers[name] ?? [])]) {
      try {
        fn(e);
      } catch (err) {
        // what the platform's log would hold
      }
    }
  };
  const update = () => {
    inUpdate = true;
    try {
      raise("update");
    } finally {
      inUpdate = false;
    }
  };
  return { raise, update };
};

// the host's defaultPuzzlePillarScript turning the pillar to its second face, the seam's answer at once (the 3D away)
const turn = (event: string) => ({
  calls: [{
    cls: "ObjectReference", fn: "PlayAnimationAndWait", kind: "action", self: PILLAR,
    args: [event, event.replace("Trigger", "Turned")], caller: "defaultPuzzlePillarScript", handler: "OnActivate",
    callerSelf: PILLAR, latent: true, stackId: 1, answered: true,
  }],
});

test("a turn the host's script made with the pillar's 3D away plays on the host's graph when the host arrives", () => {
  const realNow = Date.now;
  let now = realNow();
  Date.now = () => now;
  try {
    loaded = false;
    plays.length = 0;
    const { raise, update } = seat();
    raise("scriptCalls", turn("Trigger02"));
    update();
    assert.deepEqual(plays, []);
    // the host walks in: the pillar's 3D loads
    now += 1000;
    loaded = true;
    update();
    assert.deepEqual(plays, ["StartState02", "Trigger02"]);
  } finally {
    Date.now = realNow;
  }
});
