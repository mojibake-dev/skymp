// A served jail term's release, through JailService's own paths (src/services/services/jailService.ts): the server's
// term kept on the service, this seat's own login spawn settled (RemoteServer's ownSpawnSettled), and the update that
// runs the engine's own release on this game. The platform module is a stand-in loaded under its name; the hold's
// crime faction, which records the Papyrus calls made on it, is this file's. The release is the engine's fine path with
// the bounty cleared first: PayFine takes the bounty's gold (none) and, with goToJail, the player's next update runs
// the move out past the jail's door (40661), which clears the sentence and the prison faction.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { JailService as JailServiceClass } from "../src/services/services/jailService";

// Skyrim.esm's CrimeFactionWhiterun
const WHITERUN = 0x267ea;

// every property of the stand-in a function that returns the stand-in, so module-level reads load and a call made
// through it does nothing
const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});

// the Papyrus calls made on the hold's faction, in order
const calls: string[] = [];
const faction = {
  getFormID: () => WHITERUN,
  setCrimeGold: (n: number) => { calls.push(`setCrimeGold ${n}`); },
  setCrimeGoldViolent: (n: number) => { calls.push(`setCrimeGoldViolent ${n}`); },
  playerPayCrimeGold: (removeStolen: boolean, goToJail: boolean) => { calls.push(`playerPayCrimeGold ${removeStolen} ${goToJail}`); },
  getCrimeGoldViolent: () => 0,
  getCrimeGoldNonViolent: () => 0,
};

const platform = new Proxy({
  storage: {},
  Faction: { from: (f: unknown) => (f === faction ? faction : null) },
  Game: { getFormEx: (id: number) => (id === WHITERUN ? faction : null) },
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

const Module = require("module");
const resolve = Module._resolveFilename;
// the platform under both names the client's modules import it by
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { JailService } = require("../src/services/services/jailService") as { JailService: typeof JailServiceClass };

const sp: any = new Proxy({
  Game: platform.Game,
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

const seat = () => {
  calls.length = 0;
  const handlers: Record<string, Array<(e?: unknown) => void>> = {};
  const messages: Record<string, Array<(e?: unknown) => void>> = {};
  const controller: any = {
    on: (name: string, fn: (e?: unknown) => void) => { (handlers[name] ??= []).push(fn); },
    once: () => {},
    emitter: {
      on: (name: string, fn: (e?: unknown) => void) => { (messages[name] ??= []).push(fn); },
      emit: () => {},
    },
    lookupListener: () => anything,
  };
  const jail: any = new JailService(sp, controller);
  const update = () => (handlers.update ?? []).forEach((fn) => fn());
  const raise = (name: string, e?: unknown) => (messages[name] ?? []).forEach((fn) => fn(e));
  // the server's term as the service keeps it from its JailTerm
  const served = (isServed: boolean) => { jail.term = { faction: WHITERUN, releaseAt: 2.645, served: isServed }; };
  return { jail, update, raise, served };
};

const RELEASE = ["setCrimeGold 0", "setCrimeGoldViolent 0", "playerPayCrimeGold false true"];

test("a served term once this seat's login spawn has settled is released by the engine's own fine path, once", () => {
  const { jail, update, raise, served } = seat();
  raise("ownSpawnSettled", { how: "moved" });
  served(true);
  update();
  update();
  assert.deepEqual(calls, RELEASE);
  assert.equal(jail.term, undefined);
});

test("a served term that comes before this seat's login spawn settles waits for it, then releases", () => {
  const { update, raise, served } = seat();
  served(true);
  update();
  assert.deepEqual(calls, []);
  raise("ownSpawnSettled", { how: "loaded" });
  update();
  assert.deepEqual(calls, RELEASE);
});

test("a term not yet served releases nothing, its spawn settled or not", () => {
  const { jail, update, raise, served } = seat();
  served(false);
  update();
  raise("ownSpawnSettled", { how: "moved" });
  update();
  assert.deepEqual(calls, []);
  assert.notEqual(jail.term, undefined);
});
