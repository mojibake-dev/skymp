// A figure this seat's push knocked is claimed (src/sync/forceWatch.ts).
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { forceWatchVerdict, ForceWatch, ForcedRead } from "../src/sync/forceWatch";

test("a figure this seat's spell reached and its engine now reads knocked is claimed", () => {
  assert.equal(forceWatchVerdict({ since: 1000, now: 1400, ragdoll: true, tracked: false }), "claim");
});

test("a figure still standing within the window is watched on", () => {
  assert.equal(forceWatchVerdict({ since: 1000, now: 2900, ragdoll: false, tracked: false }), "wait");
});

test("a figure never knocked within the window leaves the watch", () => {
  assert.equal(forceWatchVerdict({ since: 1000, now: 3001, ragdoll: false, tracked: false }), "drop");
});

test("a figure already a row (followed, gripped or owned) is not claimed again", () => {
  assert.equal(forceWatchVerdict({ since: 1000, now: 1100, ragdoll: true, tracked: true }), "drop");
});

// The watch as PropService drives it: the spell's reach (magicEffectApply) and then every update, with no knock
// queued anywhere, as when a shout reaches only the other player's figure.
const PLAYER = 0x14;
const FIGURE = 0xff000de9;
const standing: ForcedRead = { figure: true, hosted: false, tracked: false, ragdoll: false };
const knocked: ForcedRead = { figure: true, hosted: false, tracked: false, ragdoll: true };

test("a shout that reaches only the other player's figure claims its body once its engine reads it knocked", () => {
  const w = new ForceWatch();
  assert.equal(w.reach(PLAYER, FIGURE, 1000), true);
  assert.deepEqual(w.update(1050, () => standing), []);
  assert.deepEqual(w.update(1300, () => knocked), [{ localId: FIGURE, verdict: "claim" }]);
  assert.equal(w.size, 0);
  assert.deepEqual(w.update(1400, () => knocked), []);
});

test("the other seat's shout (another caster) and this seat's own player are not watched", () => {
  const w = new ForceWatch();
  assert.equal(w.reach(0xff000001, FIGURE, 1000), false);
  assert.equal(w.reach(PLAYER, PLAYER, 1000), false);
  assert.equal(w.size, 0);
});

test("an actor this seat hosts (a draugr, whose graph queues no knock edge) is a row to track once its engine reads it knocked", () => {
  const w = new ForceWatch();
  assert.equal(w.reach(PLAYER, 0x380b3, 1000), true);
  assert.deepEqual(w.update(1050, () => ({ figure: false, hosted: true, tracked: false, ragdoll: false })), []);
  assert.deepEqual(w.update(1300, () => ({ figure: false, hosted: true, tracked: false, ragdoll: true })),
    [{ localId: 0x380b3, verdict: "track" }]);
});

test("the server's relayed push onto an actor this seat hosts is tracked once knocked, and one onto an actor it doesn't host is dropped", () => {
  const w = new ForceWatch();
  assert.equal(w.relayed(0xe9986, 1000), true);
  assert.equal(w.relayed(0x29602, 1000), true);
  assert.equal(w.relayed(PLAYER, 1000), false);
  const read = (localId: number): ForcedRead => ({ figure: false, hosted: localId === 0xe9986, tracked: false, ragdoll: true });
  assert.deepEqual(w.update(1100, read), [
    { localId: 0xe9986, verdict: "track" },
    { localId: 0x29602, verdict: "drop", why: "a relayed push onto an actor this seat does not host" },
  ]);
});

test("a game-file actor another seat hosts is dropped at its first read, never tracked here", () => {
  const w = new ForceWatch();
  w.reach(PLAYER, 0x29602, 1000);
  assert.deepEqual(w.update(1050, () => ({ figure: false, hosted: false, tracked: false, ragdoll: true })),
    [{ localId: 0x29602, verdict: "drop", why: "neither another player's figure nor this seat's" }]);
});

test("a figure that never falls within the window is dropped, and one that reads as no actor is dropped at once", () => {
  const w = new ForceWatch();
  w.reach(PLAYER, FIGURE, 1000);
  w.reach(PLAYER, 0xff000e10, 1000);
  const read = (localId: number): ForcedRead | null => (localId === FIGURE ? standing : null);
  assert.deepEqual(w.update(1100, read), [{ localId: 0xff000e10, verdict: "drop", why: "no actor" }]);
  assert.deepEqual(w.update(3001, read), [{ localId: FIGURE, verdict: "drop", why: "never knocked" }]);
  assert.equal(w.size, 0);
});

test("a figure that is already a row is dropped, not claimed again", () => {
  const w = new ForceWatch();
  w.reach(PLAYER, FIGURE, 1000);
  assert.deepEqual(w.update(1100, () => ({ figure: true, hosted: false, tracked: true, ragdoll: true })),
    [{ localId: FIGURE, verdict: "drop", why: "already a row" }]);
});

test("a second reach while watched keeps the first reach's time, so the window is the first hit's", () => {
  const w = new ForceWatch();
  w.reach(PLAYER, FIGURE, 1000);
  assert.equal(w.reach(PLAYER, FIGURE, 2500), false);
  assert.deepEqual(w.update(3001, () => standing), [{ localId: FIGURE, verdict: "drop", why: "never knocked" }]);
});
