// A pull of a furniture that seats nobody, through the services' own paths (src/services/services/remoteServer.ts: the
// server's OpenContainer runs the engine's activation and waits for the use's end before the closing activation;
// objectAnimationService.ts: the furniture's own graph event reported as this seat's use). The platform module is a
// stand-in loaded under its name; the frames, the game time Utility.waitMenuMode waits on, the player and the lever
// are this file's. The puzzle lever's activation sends its one graph event and never seats the puller.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { RemoteServer as RemoteServerClass } from "../src/services/services/remoteServer";
import type { ObjectAnimationService as ObjectAnimationServiceClass } from "../src/services/services/objectAnimationService";
import { MsgType } from "../src/messages";

// Bleak Falls Temple's puzzle lever (FURN 0xb7db4, LeverPushAnimatingNoPlayer)
const LEVER = 0xaa8c3;
const FURNITURE = 40;

// every property of the stand-in a function that returns the stand-in, so module-level reads load and a call made
// through it does nothing
const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});

// the frames: game time, the waits due at it, the platform's update callbacks, the furniture events its hook
// raises on the next tick
let now = 0;
let waits: Array<{ at: number; resolve: () => void }> = [];
let platformOnces: Array<() => unknown> = [];
let ticks: Array<() => void> = [];

let seated: unknown = null;
let sitState = 0;
const player = {
  getFormID: () => 0x14,
  getFurnitureReference: () => seated,
  getSitState: () => sitState,
};
let furnitureAnimation: ((e: unknown) => void) | undefined;
const lever = {
  getFormID: () => LEVER,
  getBaseObject: () => ({ getType: () => FURNITURE, getFormID: () => 0xb7db4 }),
  isActivationBlocked: () => true,
  is3DLoaded: () => true,
  getParentCell: () => ({ getFormID: () => 0x371de }),
  // the engine's furniture branch: one event to the lever's own graph, raised as furnitureAnimation on the next tick
  activate: () => {
    ticks.push(() => furnitureAnimation?.({ refrId: LEVER, event: "Lever_PlayerPush", took: true }));
    return true;
  },
};
const forms: Record<number, unknown> = { [LEVER]: lever };

const platform = new Proxy({
  storage: {},
  once: (name: string, fn: () => unknown) => {
    if (name === "update") {
      platformOnces.push(fn);
    }
  },
  on: () => {},
  Game: { getFormEx: (id: number) => forms[id] ?? null, getPlayer: () => player },
  ObjectReference: { from: (f: unknown) => (f === lever ? lever : null) },
  Actor: { from: () => null },
  Ui: { isMenuOpen: () => false },
  Utility: {
    waitMenuMode: (s: number) => new Promise<void>((resolve) => waits.push({ at: now + s * 1000, resolve })),
    wait: (s: number) => new Promise<void>((resolve) => waits.push({ at: now + s * 1000, resolve })),
  },
  FormType: { Furniture: FURNITURE, Container: 28, NPC: 43, Book: 27, Door: 29 },
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

const Module = require("module");
const resolve = Module._resolveFilename;
// the platform under both names the client's modules import it by
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { RemoteServer } = require("../src/services/services/remoteServer") as { RemoteServer: typeof RemoteServerClass };
const { ObjectAnimationService } = require("../src/services/services/objectAnimationService") as { ObjectAnimationService: typeof ObjectAnimationServiceClass };

const sp: any = new Proxy({
  settings: { "skymp5-client": {} },
  Actor: { from: () => null },
  Game: platform.Game,
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

const seat = () => {
  now = 1_000_000;
  waits = [];
  platformOnces = [];
  ticks = [];
  seated = null;
  sitState = 0;
  const handlers: Record<string, Array<(e?: unknown) => void>> = {};
  const messages: Record<string, Array<(e?: unknown) => void>> = {};
  const sent: any[] = [];
  const listeners: Record<string, unknown> = {
    HostService: { isHostedHere: () => false, epochForActor: () => 0, ownerFlips: () => 0, hostsCell: () => false },
  };
  const controller: any = {
    on: (name: string, fn: (e?: unknown) => void) => { (handlers[name] ??= []).push(fn); },
    once: (name: string, fn: (e?: unknown) => void) => { (handlers[`once:${name}`] ??= []).push(fn); },
    emitter: {
      on: (name: string, fn: (e?: unknown) => void) => { (messages[name] ??= []).push(fn); },
      emit: (name: string, e: any) => {
        if (name === "sendMessage") {
          sent.push({ at: now, message: e.message });
        }
        for (const fn of messages[name] ?? []) {
          fn(e);
        }
      },
    },
    lookupListener: (cls: { name: string }) => listeners[cls.name],
  };
  const animation: any = new ObjectAnimationService(sp, controller);
  const remote: any = new RemoteServer(sp, controller);
  listeners.ObjectAnimationService = animation;
  listeners.RemoteServer = remote;
  furnitureAnimation = (e) => handlers.furnitureAnimation?.forEach((fn) => fn(e));
  // the updates' other work, which reads the engine for other objects and sends nothing here
  animation.pollLastAnimations = () => {};
  animation.retryHeldPlays = () => {};
  remote.sweepPendingLoads = () => {};
  // one frame: game time moves on, the next tick's events arrive, the waits due resolve, the update runs
  const frame = async (ms: number) => {
    now += ms;
    const due = ticks;
    ticks = [];
    due.forEach((fn) => fn());
    const ready = waits.filter((w) => w.at <= now);
    waits = waits.filter((w) => w.at > now);
    ready.forEach((w) => w.resolve());
    const onces = [...platformOnces, ...(handlers["once:update"] ?? [])];
    platformOnces = [];
    handlers["once:update"] = [];
    for (const fn of [...onces, ...(handlers.update ?? [])]) {
      fn();
    }
    for (let i = 0; i < 4; i++) {
      await new Promise((r) => setImmediate(r));
    }
  };
  const closing = () => sent.find((s) => s.message?.t === 6 && s.message.data?.isSecondActivation === true);
  // the server's OpenContainer to this seat, the one user it admitted
  const open = () => (messages.openContainerMessage ?? []).forEach((fn) => fn({ message: { t: 0, target: LEVER } }));
  return { frame, closing, open, sent };
};

test("a pull of the puzzle lever, which seats nobody, ends its use at the lever's own event: the closing activation goes at once", async () => {
  const realNow = Date.now;
  Date.now = () => now;
  try {
    const { frame, closing, open, sent } = seat();
    const start = now;
    open();
    for (let t = 0; t < 20_000 && !closing(); t += 100) {
      await frame(100);
    }
    // the lever's own event, reported as this seat's use
    assert.ok(sent.some((s) => s.message?.t === MsgType.ObjectAnimation && s.message.event === "Lever_PlayerPush"));
    const close = closing();
    assert.ok(close, "a closing activation is sent");
    assert.equal(close.message.data.target, LEVER);
    assert.ok(close.at - start <= 3000, `the closing activation went ${close.at - start} ms after the open`);
  } finally {
    Date.now = realNow;
  }
});

test("a furniture that seats its user, its own event raised while the player waits to sit, ends its use only when the player gets up", async () => {
  const realNow = Date.now;
  Date.now = () => now;
  try {
    const { frame, closing, open } = seat();
    open();
    // the engine's sit: the player waits to sit (kWantToSit) through the furniture's own event, sits a second
    // after the open, and gets up 5 s later
    sitState = 1;
    const sitsAt = now + 1000;
    const standsAt = sitsAt + 5000;
    for (let t = 0; t < 20_000 && !closing(); t += 100) {
      if (now >= sitsAt && now < standsAt) {
        seated = lever;
        sitState = 3;
      }
      if (now >= standsAt) {
        seated = null;
        sitState = 0;
      }
      await frame(100);
    }
    const close = closing();
    assert.ok(close, "a closing activation is sent");
    assert.ok(close.at >= standsAt, `the closing activation went ${standsAt - close.at} ms before the player got up`);
  } finally {
    Date.now = realNow;
  }
});
