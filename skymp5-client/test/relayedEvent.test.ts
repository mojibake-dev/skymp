// A relayed press on the owner's game (src/sync/relayedEvent.ts), JS state alone: one whose target or presser this
// game holds no copy of is said in a line, never dropped in silence.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { relayDropLine, scriptEventLine, scriptEventOf } from "../src/sync/relayedEvent";

const LEVER = 0xaa8c3;
const GUEST = 0xff000001;

test("a relayed press whose presser has no figure here is said", () => {
  assert.strictEqual(relayDropLine(LEVER, GUEST, true, false), "relayed activation aa8c3 by ff000001 not played: no presser here");
});

test("a relayed press whose target is not here is said", () => {
  assert.strictEqual(relayDropLine(LEVER, GUEST, false, true), "relayed activation aa8c3 by ff000001 not played: no target here");
  assert.strictEqual(relayDropLine(LEVER, GUEST, false, false), "relayed activation aa8c3 by ff000001 not played: no target or presser here");
});

// the script events the owner's game sends to the object's scripts as the presser's act (fixtures
// 70-RelayedActivation-6 and -7): the guest's arrow hits a world object the host owns; the guest puts lockpicks in a
// chest the host owns
const BOW = 0x1397d;
const ARROW_PROJECTILE = 0x3be11;
const CHEST = 0x371e9;

test("an OnHit relay names its event, its source, its projectile and its flags", () => {
  assert.deepEqual(scriptEventOf({ event: 2, source: BOW, projectile: ARROW_PROJECTILE, flags: 2 }),
    { code: 2, name: "OnHit", source: BOW, projectile: ARROW_PROJECTILE, count: 0, flags: 2 });
});

test("an item event names its item and its count", () => {
  assert.deepEqual(scriptEventOf({ event: 3, source: 0xa, count: 2 }),
    { code: 3, name: "OnItemAdded", source: 0xa, projectile: 0, count: 2, flags: 0 });
  assert.strictEqual(scriptEventOf({ event: 4, source: 0xa, count: 1 })?.name, "OnItemRemoved");
  assert.strictEqual(scriptEventOf({ event: 1 })?.name, "OnActivate");
  assert.strictEqual(scriptEventOf({ event: 5, source: 0x3ae9f })?.name, "OnMagicEffectApply");
});

test("a relay with no event is an activation or an entry, and a code naming no event is refused", () => {
  assert.strictEqual(scriptEventOf({}), undefined);
  assert.strictEqual(scriptEventOf({ event: null }), undefined);
  assert.strictEqual(scriptEventOf({ event: 6 }), null);
  assert.strictEqual(scriptEventOf({ event: 2.5 }), null);
});

test("a sent event's line names the object, the presser, the event and the scripts' answer", () => {
  const hit = scriptEventOf({ event: 2, source: BOW, projectile: ARROW_PROJECTILE, flags: 2 })!;
  assert.strictEqual(scriptEventLine(CHEST, GUEST, hit, true), "relayed OnHit on 371e9 by ff000001 (source 1397d, projectile 3be11, flags 2): sent to its scripts");
  const add = scriptEventOf({ event: 3, source: 0xa, count: 2 })!;
  assert.strictEqual(scriptEventLine(CHEST, GUEST, add, false), "relayed OnItemAdded on 371e9 by ff000001 (source a, count 2): no script took it");
});
