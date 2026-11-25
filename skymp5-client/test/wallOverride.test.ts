// A fired word wall put back for a second learner, through the owner's own paths (scriptStateService.ts: the
// override's apply on the update, by name over its own read; relayedActivationService.ts: the relayed entry that
// follows it, held until the override is applied). The platform module is a stand-in loaded under its name; the
// natives' answers and the order they are called in are this file's. Bleak Falls Sanctum 0x371dd, its trigger 0x709d2.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { ScriptStateService as ScriptStateServiceClass } from "../src/services/services/scriptStateService";
import type { RelayedActivationService as RelayedActivationServiceClass } from "../src/services/services/relayedActivationService";

const TRIGGER = 0x709d2;
const SANCTUM = 0x371dd;
const GUEST = 0xff000f7b;
const GUEST_FIGURE = 0xff000801;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});

const trigger = {
  getFormID: () => TRIGGER,
  getParentCell: () => ({ getFormID: () => SANCTUM }),
};
const figure = { getFormID: () => GUEST_FIGURE };

class ObjectReference {
  static from(f: unknown) {
    return f === trigger || f === figure ? f : null;
  }
}
// the world view the guest's server id maps through to its figure here
const platform = new Proxy({
  storage: { view: { getLocalRefrId: (id: number) => (id === GUEST ? GUEST_FIGURE : 0) } },
  ObjectReference,
  FormType: { Furniture: -1 },
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { ScriptStateService } = require("../src/services/services/scriptStateService") as { ScriptStateService: typeof ScriptStateServiceClass };
const { RelayedActivationService } = require("../src/services/services/relayedActivationService") as { RelayedActivationService: typeof RelayedActivationServiceClass };

// the owner's read of the trigger: the wall fired, its script and listener in "done"
const READ = JSON.stringify({
  formId: TRIGGER,
  quiescent: true,
  fnv1a: 1,
  seatLocal: 0,
  scripts: [
    { type: "WordWallTriggerScript", state: "done", flags: 11, vars: [{ k: 5, b: true }, { k: 3, i: 60 }], names: ["::wordLearned_var", "::myStage_var"] },
    { type: "WordWallTriggerBleakFallsScript", state: "done", flags: 11, vars: [], names: [] },
  ],
});

const seat = (busyReads: number) => {
  // the natives in the order called, each with what it was given
  const calls: string[] = [];
  let busy = busyReads;
  const sp: any = new Proxy({
    settings: { "skymp5-client": {} },
    storage: {},
    Actor: { from: () => null },
    Game: { getFormEx: (id: number) => (id === TRIGGER ? trigger : id === GUEST_FIGURE ? figure : null), getPlayer: () => null },
    callNative: (cls: string, fn: string, _self: unknown, ...args: unknown[]) => {
      if (cls !== "TESModPlatform") {
        return anything;
      }
      if (fn === "SerializeScriptState" && args[0] === trigger) {
        calls.push("read");
        if (busy > 0) {
          busy--;
          return JSON.stringify({ formId: TRIGGER, quiescent: false, busy: "a stack on WordWallTriggerScript" });
        }
        return READ;
      }
      if (fn === "ApplyScriptState" && args[0] === trigger) {
        calls.push(`apply ${String(args[1])}`);
        return JSON.stringify({ applied: true, scripts: [], blanked: [] });
      }
      if (fn === "SendTriggerEvent") {
        calls.push(`${String(args[1])} by ${(args[2] as { getFormID: () => number }).getFormID().toString(16)}`);
        return JSON.stringify({ sent: true, scripts: 2 });
      }
      return anything;
    },
  }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

  const updates: Array<() => void> = [];
  let onces: Array<() => void> = [];
  const handlers: Record<string, (e: unknown) => void> = {};
  const listeners: Record<string, unknown> = {
    SpSnippetService: { seqOf: () => undefined, raiseSeq: () => {} },
    HostService: { epochForActor: () => 0, ownerFlips: () => 0, hostsCell: () => true, isHostedHere: () => true },
    PuppetService: { figureOf: () => GUEST_FIGURE },
    RemoteServer: { getMyActorRemoteId: () => 0xff000f7a, localIdToRemoteId: () => 0 },
  };
  const controller: any = {
    on: (name: string, fn: () => void) => {
      if (name === "update") {
        updates.push(fn);
      }
    },
    once: (name: string, fn: () => void) => {
      if (name === "update") {
        onces.push(fn);
      }
    },
    emitter: { on: (name: string, fn: (e: unknown) => void) => { handlers[name] = fn; }, emit: () => {} },
    lookupListener: (cls: { name: string }) => listeners[cls.name],
  };
  const state: any = new ScriptStateService(sp, controller);
  const relayed: any = new RelayedActivationService(sp, controller);
  listeners.ScriptStateService = state;
  listeners.RelayedActivationService = relayed;
  state.persistMarks = () => {};
  relayed.isOwnPlayer = () => false;
  relayed.nameEntryActing = () => {};
  const tick = () => {
    const now = onces;
    onces = [];
    for (const fn of updates) {
      fn();
    }
    for (const fn of now) {
      fn();
    }
  };
  const override = handlers["scriptStateOverrideMessage"];
  const entry = handlers["relayedActivationMessage"];
  return { calls, tick, override, entry };
};

const OVERRIDE = {
  t: 89,
  refrId: TRIGGER,
  scripts: [
    { type: "WordWallTriggerScript", state: "", vars: [{ name: "::WordLearned_var", k: 5, b: false }] },
    { type: "WordWallTriggerBleakFallsScript", state: "Listening", vars: [] },
  ],
};
const ENTRY = { t: 33, target: TRIGGER, caster: GUEST, entry: 1 };
const APPLIED =
  "apply " +
  JSON.stringify([
    { type: "WordWallTriggerScript", state: "", flags: 11, vars: [{ k: 5, b: false }, { k: 3, i: 60 }] },
    { type: "WordWallTriggerBleakFallsScript", state: "Listening", flags: 11, vars: [] },
  ]);

test("the owner's game puts the override over its own read by name, then sends the guest's relayed entry to the trigger", () => {
  const { calls, tick, override, entry } = seat(0);
  assert.equal(typeof override, "function", "the owner's game takes the override");
  override({ message: OVERRIDE });
  entry({ message: ENTRY });
  for (let i = 0; i < 4; i++) {
    tick();
  }
  assert.deepEqual(calls, ["read", APPLIED, `OnTriggerEnter by ${GUEST_FIGURE.toString(16)}`]);
});

test("a read the wall's scripts keep busy holds the guest's entry until the override is applied", () => {
  const { calls, tick, override, entry } = seat(1);
  assert.equal(typeof override, "function", "the owner's game takes the override");
  override({ message: OVERRIDE });
  entry({ message: ENTRY });
  for (let i = 0; i < 3; i++) {
    tick();
  }
  assert.deepEqual(calls, ["read"], "the entry waits while the read is busy");
  // past the busy retry's wait
  const waitUntil = Date.now() + 300;
  while (Date.now() < waitUntil) {
    // the retry's 250 ms
  }
  for (let i = 0; i < 4; i++) {
    tick();
  }
  assert.deepEqual(calls, ["read", "read", APPLIED, `OnTriggerEnter by ${GUEST_FIGURE.toString(16)}`]);
});
