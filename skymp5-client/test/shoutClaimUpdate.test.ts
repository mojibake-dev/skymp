// The shouter's claim of the other player's body through PropService's own update path
// (src/services/services/propService.ts: the magicEffectApply handler, then onUpdate and watchForced), with no knock
// queued, as when a shout reaches only the other player's figure. The platform module is a stand-in loaded under its
// name; the engine reads around the claim (the body's class, its row, this seat's id) are this file's, and onUpdate,
// watchForced and the event handler are the service's own.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { PropService as PropServiceClass } from "../src/services/services/propService";

const PLAYER = 0x14;
const FIGURE = 0xff000de9;
const FIGURE_REMOTE = 0xff000001;

// every property of the stand-in a function that returns the stand-in, so module-level reads load and a call made
// through it does nothing
const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
// the platform's storage holds the world view, which maps the figure's local id to its server id
const storage: Record<string, unknown> = {
  view: { getRemoteRefrId: (localId: number) => (localId === FIGURE ? FIGURE_REMOTE + 0x100000000 : 0), getLocalRefrId: () => 0 },
};
const platform = new Proxy({ storage }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

const Module = require("module");
const resolve = Module._resolveFilename;
// the platform under both names the client's modules import it by
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { PropService } = require("../src/services/services/propService") as { PropService: typeof PropServiceClass };

const figureActor = { getBaseObject: () => ({ getFormID: () => 0x7 }), getFormID: () => FIGURE, isDead: () => false };
// a draugr this seat hosts (a game-file actor whose graph is no humanoid's, so the action hook queues no knock for it)
const DRAUGR = 0x380b3;
const draugrActor = { getBaseObject: () => ({ getFormID: () => 0x4a59a }), getFormID: () => DRAUGR, isDead: () => false };
// a draugr another seat hosts, a puppet here
const PUPPET = 0x380b5;
const puppetActor = { getBaseObject: () => ({ getFormID: () => 0x567c8 }), getFormID: () => PUPPET, isDead: () => false };
const actors: Record<number, unknown> = { [FIGURE]: figureActor, [DRAUGR]: draugrActor, [PUPPET]: puppetActor };
const sp: any = new Proxy({
  Actor: { from: (f: unknown) => (f === figureActor || f === draugrActor || f === puppetActor ? f : null) },
  Game: { getFormEx: (id: number) => actors[id] ?? null, getPlayer: () => null },
}, { get: (t, k) => (k in t ? (t as any)[k] : anything) });

const service = () => {
  const handlers: Record<string, (e?: unknown) => void> = {};
  const messages: Record<string, (e?: unknown) => void> = {};
  const controller: any = {
    on: (name: string, fn: (e?: unknown) => void) => { handlers[name] = fn; },
    emitter: { on: (name: string, fn: (e?: unknown) => void) => { messages[name] = fn; }, emit: () => {} },
  };
  const s: any = new PropService(sp, controller);
  const claims: Array<{ refr: number; touch: number }> = [];
  // the steps of the update that read the engine for other bodies, and the claim's own send
  for (const step of ["applyImpulseRule", "applyWorldBodyContacts", "sweepEngineRefs", "rescan", "readHolds"]) {
    s[step] = () => {};
  }
  s.enabled = () => true;
  s.myId = () => 7;
  s.ownedHere = () => false;
  s.bodyClass = (localId: number) => (localId === FIGURE || localId === DRAUGR || localId === PUPPET
    ? { ragdoll: true, parts: [0], bones: [], cut: false } : null);
  // the rows made, by local id, and whether this seat's push made them (rowInPlace's and rowOrRefusal's last argument)
  const rows: Array<{ localId: number; pushed: boolean }> = [];
  const rowFor = (r: { localId: number }, _now: number, _why: string, _gripped?: boolean, pushed?: boolean) => {
    rows.push({ localId: r.localId, pushed: pushed === true });
    return { refr: r.localId === FIGURE ? FIGURE_REMOTE : r.localId, localId: r.localId };
  };
  s.rowInPlace = rowFor;
  s.rowOrRefusal = rowFor;
  s.hostsHere = (localId: number) => localId === DRAUGR;
  s.othersHere = (localId: number) => localId === PUPPET;
  s.claim = (prop: { refr: number }, touch: number) => { claims.push({ refr: prop.refr, touch }); };
  return { handlers, messages, claims, rows, s };
};

const shoutReaches = (handlers: Record<string, (e?: unknown) => void>) =>
  handlers.magicEffectApply({ caster: { getFormID: () => PLAYER }, target: { getFormID: () => FIGURE } });

test("a shout that reaches only the other player's figure is claimed by touch 3 at the next update, with no knock queued", () => {
  const { handlers, claims } = service();
  shoutReaches(handlers);
  handlers.update();
  assert.deepEqual(claims, [{ refr: FIGURE_REMOTE, touch: 3 }]);
});

test("a shout that also knocks an NPC this seat hosts (a knock queued) claims the figure too", () => {
  const { handlers, claims, s } = service();
  s.trackKnocks = () => {};
  shoutReaches(handlers);
  // the knock edge the action hook queues for a hosted NPC (trackKnocks itself is the old path's, read elsewhere)
  s.knocks.push(0x29602);
  handlers.update();
  assert.deepEqual(claims, [{ refr: FIGURE_REMOTE, touch: 3 }]);
});

test("a shout that reaches a draugr this seat hosts makes its body a row once its engine reads it knocked, with no knock queued and no claim", () => {
  const { handlers, claims, rows } = service();
  handlers.magicEffectApply({ caster: { getFormID: () => PLAYER }, target: { getFormID: () => DRAUGR } });
  handlers.update();
  assert.deepEqual(rows, [{ localId: DRAUGR, pushed: true }]);
  assert.deepEqual(claims, []);
});

test("the server's relayed push onto a draugr this seat hosts (another seat's shout) makes its body a row once its engine reads it knocked", () => {
  const { handlers, messages, claims, rows } = service();
  // the snippet OnPushActor sends the host: ObjectReference.PushActorAway on the caster, the target a game-file actor by
  // its long id (SpSnippet::MakeLongFormId)
  messages.spSnippetMessage({ message: { class: "ObjectReference", function: "PushActorAway", selfId: FIGURE_REMOTE,
    arguments: [{ formId: DRAUGR + 0x100000000, type: "Actor" }, 15], snippetIdx: 1 } });
  handlers.update();
  assert.deepEqual(rows, [{ localId: DRAUGR, pushed: true }]);
  assert.deepEqual(claims, []);
});

test("a shout that reaches a draugr another seat hosts claims its body by touch 3 once its engine reads it knocked, with no knock queued", () => {
  const { handlers, claims, rows } = service();
  handlers.magicEffectApply({ caster: { getFormID: () => PLAYER }, target: { getFormID: () => PUPPET } });
  handlers.update();
  assert.deepEqual(rows, [{ localId: PUPPET, pushed: true }]);
  assert.deepEqual(claims, [{ refr: PUPPET, touch: 3 }]);
});

test("another seat's shout on the figure is not this seat's to claim", () => {
  const { handlers, claims } = service();
  handlers.magicEffectApply({ caster: { getFormID: () => 0xff000002 }, target: { getFormID: () => FIGURE } });
  handlers.update();
  assert.deepEqual(claims, []);
});
