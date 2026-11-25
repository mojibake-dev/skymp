// A served graph state on a guest, through the services' own paths (objectAnimationService.ts: the relay's play and
// its keep; scriptStateService.ts: the apply of the object's script record, which asks graphRestore whether the
// owner's graph state is restored). The platform module is a stand-in loaded under its name; the word's graph, which
// takes or refuses each event, and the platform natives' answers are this file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { ObjectAnimationService as ObjectAnimationServiceClass } from "../src/services/services/objectAnimationService";
import type { ScriptStateService as ScriptStateServiceClass } from "../src/services/services/scriptStateService";

// Bleak Falls Sanctum's word, ACTI NorWordWallWordFus (graph TiidBehavior.hkx)
const WORD = 0x709d1;
const SANCTUM = 0x371dd;

// every property of the stand-in a function that returns the stand-in, so module-level reads load and a call made
// through it does nothing
const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});

// whether the word's graph takes the next event, and what it was sent
let takes = false;
const sentToGraph: string[] = [];
const word = {
  getFormID: () => WORD,
  is3DLoaded: () => true,
  playAnimation: (event: string) => {
    sentToGraph.push(event);
    return takes;
  },
  getParentCell: () => ({ getFormID: () => SANCTUM }),
  getBaseObject: () => ({ getFormID: () => 0x7098e, getType: () => 0 }),
};

const platform = new Proxy({
  storage: {},
  ObjectReference: { from: (f: unknown) => (f === word ? word : null) },
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
const { ObjectAnimationService } = require("../src/services/services/objectAnimationService") as { ObjectAnimationService: typeof ObjectAnimationServiceClass };
const { ScriptStateService } = require("../src/services/services/scriptStateService") as { ScriptStateService: typeof ScriptStateServiceClass };

// the natives the apply calls: the record's scripts taken, the graph state restored, each restore counted
let restores = 0;
const sp: any = new Proxy({
  settings: { "skymp5-client": {} },
  storage: {},
  Actor: { from: () => null },
  Game: { getFormEx: (id: number) => (id === WORD ? word : null), getPlayer: () => null },
  callNative: (cls: string, fn: string) => {
    if (cls === "TESModPlatform" && fn === "ApplyScriptState") {
      return JSON.stringify({ applied: true, scripts: [], blanked: [] });
    }
    if (cls === "TESModPlatform" && fn === "ApplyGraphState") {
      restores++;
      return JSON.stringify({ applied: true, how: "restored", bytes: 216 });
    }
    return anything;
  },
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

const seat = () => {
  takes = false;
  sentToGraph.length = 0;
  restores = 0;
  // the newest relay's seq this seat took for the word
  let lastSeq: number | undefined;
  const listeners: Record<string, unknown> = {
    SpSnippetService: { seqOf: () => lastSeq, raiseSeq: () => {} },
    HostService: { epochForActor: () => 0, ownerFlips: () => 0, hostsCell: () => false, isHostedHere: () => false },
  };
  const controller: any = {
    on: () => {},
    once: () => {},
    emitter: { on: () => {}, emit: () => {} },
    lookupListener: (cls: { name: string }) => listeners[cls.name],
  };
  const animation: any = new ObjectAnimationService(sp, controller);
  const state: any = new ScriptStateService(sp, controller);
  listeners.ObjectAnimationService = animation;
  listeners.ScriptStateService = state;
  // the create word subscribed this seat to the word
  animation.subscribed.add(WORD);
  state.persistMarks = () => {};
  state.finish = () => {};
  // the host's script's relay of the word's event, seq `seq`, played on this seat's copy
  const relay = (event: string, seq: number) => {
    lastSeq = seq;
    animation.play(WORD, event, seq);
  };
  // the word's script record of seq `seq`, its view `view` and the owner's graph state, applied as a guest
  const record = (view: string, seq: number) => {
    const h = {
      m: { seq }, scripts: "", view, receivedAt: 0, foundAt: 0, marked: false, carried: [], claim: 0,
      phase: "apply", nextTryAt: 0, appliedAt: 0, reason: "", viewTries: 0, vars: [], graph: "00d8",
    };
    return state.apply(WORD, h, word, 1000, 0, undefined);
  };
  return { relay, record };
};

test("a relay the word's graph refused leaves the owner's graph state of its seq to be restored", () => {
  const { relay, record } = seat();
  takes = false;
  relay("Dark1Wild", 1);
  assert.deepEqual(sentToGraph, ["Dark1Wild"]);
  record("Dark1Wild", 1);
  assert.equal(restores, 1);
});

test("a relay the word's graph took stands for the owner's graph state of its seq: the state is not restored, so the act never starts twice", () => {
  const { relay, record } = seat();
  takes = true;
  relay("Dark2Wild", 7);
  record("Dark2Wild", 7);
  assert.equal(restores, 0);
});
