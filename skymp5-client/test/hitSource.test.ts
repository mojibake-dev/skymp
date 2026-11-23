// What a hit's source is, for whether the hit is reported (src/sync/hitSource.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { hitSourceKind } from "../src/sync/hitSource";

// Skyrim.esm (thuum lab/esm.py): StaffEnchFlames, the Staff of Flames' enchantment
const staffEnchFlames = 0x4deda;
const plain = { weapon: false, spell: false, scroll: false, enchantment: 0, staffEnchantments: [] as number[] };

test("a Staff of Flames' fire on the other player is reported, as its staff's enchantment", () => {
  assert.strictEqual(hitSourceKind({ ...plain, enchantment: staffEnchFlames, staffEnchantments: [staffEnchFlames] }), "staff");
  // the staff in the left hand counts the same
  assert.strictEqual(hitSourceKind({ ...plain, enchantment: staffEnchFlames, staffEnchantments: [0, staffEnchFlames] }), "staff");
});

test("an enchantment no staff in the aggressor's hands carries is no staff's hit", () => {
  // a weapon's enchantment arrives as its weapon's hit, which the server runs whole
  assert.strictEqual(hitSourceKind({ ...plain, enchantment: 0x49bb7, staffEnchantments: [staffEnchFlames] }), "none");
  assert.strictEqual(hitSourceKind({ ...plain, enchantment: staffEnchFlames, staffEnchantments: [] }), "none");
});

test("a weapon, a spell and a scroll are reported as before", () => {
  assert.strictEqual(hitSourceKind({ ...plain, weapon: true }), "weapon");
  assert.strictEqual(hitSourceKind({ ...plain, spell: true }), "spell");
  assert.strictEqual(hitSourceKind({ ...plain, scroll: true }), "scroll");
  assert.strictEqual(hitSourceKind(plain), "none");
});
