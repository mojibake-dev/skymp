// The name a stack keeps as its own (src/sync/itemName.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { ownName } from "../src/sync/itemName";

// the engine's display name of a tempered dagger, as the dropper's game read it
test("a dropped Fine iron dagger keeps no name of its own, so the picker's game shows one (Fine)", () => {
  assert.strictEqual(ownName("Iron Dagger (Fine)", 1.1, "Iron Dagger"), undefined);
  assert.strictEqual(ownName("Iron Helmet (Fine)", 1.1, "Iron Helmet"), undefined);
});

test("a player's own name on a tempered item keeps the name and leaves the temper label to the engine", () => {
  assert.strictEqual(ownName("Wolfbane (Fine)", 1.1, "Iron Dagger"), "Wolfbane");
  // a name of the player's own that has its own brackets loses only the engine's one label
  assert.strictEqual(ownName("Blade (Old) (Fine)", 1.1, "Iron Dagger"), "Blade (Old)");
});

test("an enchanted dagger the player named keeps its name, untempered", () => {
  assert.strictEqual(ownName("testEbony Dagger", undefined, "Ebony Dagger"), "testEbony Dagger");
  assert.strictEqual(ownName("Blade (Old)", undefined, "Iron Dagger"), "Blade (Old)");
});

test("no name, or the base's own, is no name of the stack's", () => {
  assert.strictEqual(ownName(undefined, 1.1, "Iron Dagger"), undefined);
  assert.strictEqual(ownName("", undefined, "Iron Dagger"), undefined);
  assert.strictEqual(ownName("Iron Dagger", undefined, "Iron Dagger"), undefined);
});
