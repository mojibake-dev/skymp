// The party's level this game is told (the server's PartyLevel), handed by
// PartyLevelService (src/services/services/partyLevelService.ts) to the
// platform's setPartyLevel, which the engine's level reads for every
// PC-level-mult NPC on this game. The platform module is a stand-in loaded
// under its name, as in test/jailTerm.test.ts.
import { test } from "node:test";
import * as assert from "node:assert/strict";

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
const stub = (own: Record<string, unknown>): any =>
  new Proxy(own, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

// the levels handed to the platform, in order
const handed: number[] = [];
const sp: any = stub({
  setPartyLevel: (level: number) => {
    handed.push(level);
    return true;
  },
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
const { PartyLevelService } = require("../src/services/services/partyLevelService");

// a PartyLevelService on a fake controller; tell(level) delivers the
// server's PartyLevel to the handler it registered
const service = () => {
  const messages: Record<string, (e: any) => void> = {};
  const controller: any = {
    on: () => {},
    once: () => {},
    emitter: { on: (name: string, fn: (e: any) => void) => { messages[name] = fn; }, emit: () => {} },
  };
  new PartyLevelService(sp, controller);
  return (level: unknown) => messages["partyLevelMessage"]?.({ message: { t: 90, level } });
};

test("each party level the server tells is handed to the engine once", () => {
  handed.length = 0;
  const tell = service();
  tell(27);
  tell(27);
  tell(28);
  tell(14);
  assert.deepEqual(handed, [27, 28, 14]);
});

test("a level that is not a whole number of at least 1 is never handed on", () => {
  handed.length = 0;
  const tell = service();
  tell(0);
  tell(-3);
  tell(2.5);
  tell("27");
  tell(undefined);
  tell(70000);
  assert.deepEqual(handed, []);
  tell(19);
  assert.deepEqual(handed, [19]);
});
