// The bases an own script row on this seat's player holds against the inventory apply (src/sync/ownItemHold.ts):
// a quest's gift stays through the 5 s apply until the server's record carries it.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { ownItemHolds } from "../src/sync/ownItemHold";

const gold = 0xf;
// DLC1ArmorVampireBoots (Dawnguard.esm 0x0200e8de in the load order)
const vampireBoots = 0x0200e8de;

test("a script's AddItem on this seat's player holds its base's surplus, a RemoveItem its deficit", () => {
  assert.deepStrictEqual(ownItemHolds({ self: 0x14, cls: "ObjectReference", fn: "AddItem", family: "own", item: gold }), [{ formId: gold, keep: "surplus" }]);
  assert.deepStrictEqual(ownItemHolds({ self: 0x14, cls: "ObjectReference", fn: "RemoveItem", family: "own", item: gold }), [{ formId: gold, keep: "deficit" }]);
  assert.deepStrictEqual(ownItemHolds({ self: 0x14, cls: "ObjectReference", fn: "AddItem", family: "own", item: 0x1, landed: [gold, 0x2] }), [{ formId: gold, keep: "surplus" }, { formId: 0x2, keep: "surplus" }]);
});

test("a vampire start's boots a script equips on this seat's player stay on through the inventory apply", () => {
  assert.deepStrictEqual(ownItemHolds({ self: 0x14, cls: "Actor", fn: "EquipItem", family: "own", item: vampireBoots }), [{ formId: vampireBoots, keep: "surplus" }]);
});

test("another actor's call, or a row the table keeps for no own record, holds nothing", () => {
  assert.deepStrictEqual(ownItemHolds({ self: 0x13bbe, cls: "ObjectReference", fn: "AddItem", family: "own", item: gold }), []);
  assert.deepStrictEqual(ownItemHolds({ self: 0x14, cls: "ObjectReference", fn: "AddItem", family: undefined, item: gold }), []);
  assert.deepStrictEqual(ownItemHolds({ self: 0x14, cls: "ObjectReference", fn: "AddItem", family: "own", item: 0 }), []);
});
