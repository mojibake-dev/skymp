// The player's inventory apply's diff (src/sync/inventoryDiff.ts), JS state
// alone: the record less the game, a positive count added and a negative one
// removed. The player's own apply leaves worn flags out (ignoreWorn).
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { Entry, getDiff } from "../src/sync/inventoryDiff";

// Skyrim.esm (lab/esm.py on 1.6.1170): IronDagger, ArmorIronHelmet
const ironDagger = 0x1397e;
const ironHelmet = 0x12e4d;
// the record holds health as the wire's f32; the engine's read is one decimal
const recordFine = Math.fround(1.1);
const engineFine = 1.1;

const diff = (record: Entry[], game: Entry[], ignoreWorn = true) => getDiff({ entries: record }, { entries: game }, ignoreWorn).entries;

test("a dagger tempered at the grindstone stays Fine: the record's tempered row is the engine's stack, nothing removed or re-added", () => {
  assert.notStrictEqual(recordFine, engineFine);
  assert.deepStrictEqual(diff([{ baseId: ironDagger, count: 1, health: recordFine }], [{ baseId: ironDagger, count: 1, health: engineFine }]), []);
});

test("the Fine dagger equipped: the player's apply leaves the worn flag out, so the equip changes nothing", () => {
  const record: Entry[] = [{ baseId: ironDagger, count: 1, health: recordFine }];
  const game: Entry[] = [{ baseId: ironDagger, count: 1, health: engineFine, worn: true }];
  assert.deepStrictEqual(diff(record, game), []);
  // with the worn flag in the match, the same two rows would differ
  assert.notDeepStrictEqual(diff(record, game, false), []);
});

test("a helmet tempered at the armor workbench and equipped: the same, nothing removed or re-added", () => {
  assert.deepStrictEqual(diff([{ baseId: ironHelmet, count: 1, health: recordFine }], [{ baseId: ironHelmet, count: 1, health: engineFine }]), []);
  assert.deepStrictEqual(diff([{ baseId: ironHelmet, count: 1, health: recordFine }], [{ baseId: ironHelmet, count: 1, health: engineFine, worn: true }]), []);
});

test("a temper the record does not hold is still a difference: a plain row and a Fine stack, or two tempers, never match", () => {
  assert.deepStrictEqual(diff([{ baseId: ironDagger, count: 1 }], [{ baseId: ironDagger, count: 1, health: engineFine }]), [
    { baseId: ironDagger, count: 1 },
    { baseId: ironDagger, count: -1, health: engineFine },
  ]);
  assert.deepStrictEqual(diff([{ baseId: ironDagger, count: 1, health: Math.fround(1.2) }], [{ baseId: ironDagger, count: 1, health: engineFine }]), [
    { baseId: ironDagger, count: 1, health: Math.fround(1.2) },
    { baseId: ironDagger, count: -1, health: engineFine },
  ]);
});
