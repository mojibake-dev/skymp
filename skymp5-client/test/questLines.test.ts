// The quest-stage service's per-event trace (src/services/services/questStageService.ts sayCause), through the
// service itself. A mod's own script that starts and stops a quest twice a second (Reading List's book radar, a
// manager quest's helper) filled a seat's console with one line per event, about ten thousand a sitting; the lab
// driver's own quest lines already keep one a quest and kind each ten seconds (lab/driver logQuestEvent). The
// trace keeps the same rule: a quest's first line of a kind always, then one each ten seconds carrying the count of
// the lines it stands for. The platform module is a stand-in loaded under its name; its console is this file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { QuestStageService as QuestStageServiceClass } from "../src/services/services/questStageService";

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
const lines: string[] = [];
const platform = new Proxy({
  storage: {},
  settings: { "skymp5-client": {} },
  printConsole: (...args: unknown[]) => { lines.push(args.map(String).join(" ")); },
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { QuestStageService } = require("../src/services/services/questStageService") as { QuestStageService: typeof QuestStageServiceClass };

const service = () => {
  const controller: any = new Proxy({
    on: () => {},
    once: () => {},
    emitter: { on: () => {}, emit: () => {} },
    lookupListener: () => anything,
  }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  return new QuestStageService(platform as any, controller) as any;
};
const seatCause = { world: false, root: "RL_ManagerScript.OnUpdate", rootSelf: 0 };
const of = (needle: string) => lines.filter((l) => l.includes(needle));

test("a quest started a hundred times within ten seconds says its start once, then once a ten seconds with the count", () => {
  const s = service();
  lines.length = 0;
  const realNow = Date.now;
  let t = 1_000_000;
  Date.now = () => t;
  try {
    for (let i = 0; i < 100; ++i) {
      s.sayCause(0x30002859, "start", seatCause);
      t += 50;
    }
    // five seconds in: the first line alone
    assert.equal(of("quest 30002859 start").length, 1);
    // another quest, and the same quest's stop, each its own first line
    s.sayCause(0x2b01e2c7, "start", seatCause);
    s.sayCause(0x30002859, "stop", seatCause);
    assert.equal(of("quest 2b01e2c7 start").length, 1);
    assert.equal(of("quest 30002859 stop").length, 1);
    // past ten seconds: one more line, standing for the ones held
    t += 6_000;
    s.sayCause(0x30002859, "start", seatCause);
    const said = of("quest 30002859 start");
    assert.equal(said.length, 2);
    assert.match(said[1], /99 more in the last 10 s/);
  } finally {
    Date.now = realNow;
  }
});
