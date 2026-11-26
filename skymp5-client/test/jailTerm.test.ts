// The jail term this game's player serves, as the server's record holds it,
// through JailService's own handlers (src/services/services/jailService.ts):
// the server's JailTerm word kept, and the jail bed's serve answered with the
// time left on the shared clock. The platform module is a stand-in loaded
// under its name, as in test/shoutClaimUpdate.test.ts; this game's clock
// (Utility.GetCurrentGameTime, which the shared clock sets here) and its
// TimeScale global are this file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { JailService as JailServiceClass } from "../src/services/services/jailService";

// CrimeFactionWhiterun
const WHITERUN = 0x267ea;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
const stub = (own: Record<string, unknown>): any =>
  new Proxy(own, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

let gameDays = 20;
const shown: string[] = [];
const sp: any = stub({
  Utility: stub({ getCurrentGameTime: () => gameDays }),
  Debug: stub({ notification: (text: string) => { shown.push(text); } }),
  // Skyrim.esm's TimeScale 20: a game hour is three real minutes
  GlobalVariable: stub({ from: () => ({ getValue: () => 20 }) }),
  Game: stub({ getFormEx: () => ({}) }),
  // the platform's storage, where the service keeps the told term; each service below starts with its own
  storage: {},
});
const platform = stub({ storage: {}, settings: {} });

const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { JailService } = require("../src/services/services/jailService") as { JailService: typeof JailServiceClass };

// a JailService on a fake controller: its platform events and its server
// messages as the handlers it registered
const service = () => {
  const events: Record<string, (e: any) => void> = {};
  const messages: Record<string, (e: any) => void> = {};
  const controller: any = {
    on: (name: string, fn: (e: any) => void) => { events[name] = fn; },
    once: () => {},
    emitter: { on: (name: string, fn: (e: any) => void) => { messages[name] = fn; }, emit: () => {} },
  };
  sp.storage = {};
  const s: any = new JailService(sp, controller);
  const told = (faction: number, releaseAt: number, served: boolean) =>
    messages["jailTermMessage"]?.({ message: { t: 88, faction, releaseAt, served } });
  const serve = () => events["servePrisonTime"]?.({ factionId: WHITERUN, days: 3 });
  const arrest = (days: number) => events["arrest"]?.({ faction: {}, factionId: WHITERUN, days });
  return { s, told, serve, arrest };
};

test("a prisoner whose game never saw its arrest is answered at the bed with the time left on the server's term", () => {
  const { told, serve } = service();
  gameDays = 22.5;
  told(WHITERUN, 23, false);
  shown.length = 0;
  serve();
  assert.equal(shown.length, 1);
  assert.match(shown[0], /about 12 hour\(s\)/);
  assert.match(shown[0], /about 36 real minute\(s\)/);
});

test("the bed answers from the record's end, not from this game's own clock at the arrest", () => {
  const { told, serve, arrest } = service();
  gameDays = 20;
  arrest(3);
  // the record's end, as the server set it on the shared clock
  told(WHITERUN, 21.5, false);
  gameDays = 20.5;
  shown.length = 0;
  serve();
  assert.equal(shown.length, 1);
  assert.match(shown[0], /about 24 hour\(s\)/);
});

test("the newest term the server told is the one kept, whole", () => {
  const { s, told } = service();
  told(WHITERUN, 21.5, false);
  told(WHITERUN, 21.5, true);
  assert.deepEqual(s.term, { faction: WHITERUN, releaseAt: 21.5, served: true });
});

test("a served term's bed answer names no time left", () => {
  const { told, serve } = service();
  gameDays = 22;
  told(WHITERUN, 21.5, true);
  shown.length = 0;
  serve();
  assert.equal(shown.length, 1);
  assert.doesNotMatch(shown[0], /left/);
});
