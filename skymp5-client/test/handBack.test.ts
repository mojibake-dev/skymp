// A thrown player's own game after the hand-back, through PropService's own update path
// (src/services/services/propService.ts: trackStreams, tick, untrack, trackKnocks and trackOwnBody). The forcer's
// stream or word hands this seat's player to the forcer; the rescan untracks the row as out of reach once the word
// names this seat again, before the rest's wait ends; then this seat's engine still reads its player down. The
// platform module is a stand-in loaded under its name, as in test/shoutClaimUpdate.test.ts; the engine reads around
// the hand (the body's class, its kind, its row) are this file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { PropService as PropServiceClass } from "../src/services/services/propService";

const PLAYER = 0x14;
const ME = 7;
const FORCER = 9;

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
const { PropService } = require("../src/services/services/propService") as { PropService: typeof PropServiceClass };

const service = () => {
  const handlers: Record<string, (e?: unknown) => void> = {};
  const controller: any = {
    on: (name: string, fn: (e?: unknown) => void) => { handlers[name] = fn; },
    emitter: { on: () => {}, emit: () => {} },
    lookupListener: () => anything,
  };
  let player: unknown = null;
  const sp: any = new Proxy({
    Game: { getPlayer: () => player, getFormEx: () => null },
  }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  const s: any = new PropService(sp, controller);
  const claims: Array<{ refr: number; touch: number }> = [];
  for (const step of ["applyImpulseRule", "applyWorldBodyContacts", "sweepEngineRefs", "rescan", "readHolds", "feed",
    "readSimulated", "claimIslands", "sendDue"]) {
    s[step] = step === "readSimulated" ? () => new Map() : () => {};
  }
  s.enabled = () => true;
  s.myId = () => ME;
  // this seat's engine: its player down (ragdolled) or upright
  const engine = { down: false };
  s.bodyClass = (localId: number) => (localId === PLAYER ? { ragdoll: engine.down, parts: [], bones: [], cut: false } : null);
  s.kindOf = (localId: number) => (localId === PLAYER && engine.down ? "world" : "none");
  s.trackInPlace = (refr: number, localId: number) => {
    const prop = (PropService as any).newProp(refr, localId, -1, 0, 0, 0, true, 18);
    s.props.set(refr, prop);
    s.byLocal.set(localId, refr);
    return prop;
  };
  s.claim = (prop: { refr: number }, touch: number) => { claims.push({ refr: prop.refr, touch }); };
  const knock = () => {
    s.knocks.push(PLAYER);
    handlers.update();
  };
  return { handlers, claims, s, engine, knock, setPlayer: (p: unknown) => { player = p; } };
};

test("a grip's thrown player, untracked by the rescan before the rest's wait, is not claimed by its own game while it lies down", () => {
  const { handlers, claims, s, engine, knock } = service();
  // the gripper's stream of this seat's player arrives: the row is made handed
  engine.down = true;
  s.streamsUntracked.push({ message: { refr: ME }, at: 0 });
  handlers.update();
  const row = s.props.get(ME);
  assert.ok(row && row.handed, "the gripper's stream hands this seat's player");
  // the rescan's untrack once the word names this seat, the follow's rest not yet rendered
  s.untrack(row, "out of reach");
  // its engine still reads it down: no stream of its own get-up
  knock();
  assert.deepEqual(claims, []);
  // once it reads upright, a later knock of its own is its own game's again
  engine.down = false;
  knock();
  engine.down = true;
  knock();
  assert.deepEqual(claims, [{ refr: ME, touch: 3 }]);
});

test("a shout's thrown player, its own knock claimed first and the shouter's word after, is not claimed again after the rescan's untrack", () => {
  const { handlers, claims, s, engine, knock, setPlayer } = service();
  // its own engine read it knocked first: its own row, claimed by touch 3
  engine.down = true;
  knock();
  assert.deepEqual(claims, [{ refr: ME, touch: 3 }]);
  claims.length = 0;
  const row = s.props.get(ME);
  // the track's grace over (the test's clock starts near 0)
  row.trackedAt = -10000;
  // the shouter's word names the shouter: the update's tick hands the row
  setPlayer({});
  s.owners.set(ME, { player: FORCER, epoch: 2, at: 1 });
  handlers.update();
  assert.ok(row.handed, "the shouter's word hands this seat's player");
  setPlayer(null);
  s.untrack(row, "out of reach");
  knock();
  assert.deepEqual(claims, []);
});
