// The worn rows a dress laid that the engine holds unworn (src/sync/wornDress.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { toEquipAfterDress } from "../src/sync/wornDress";

const FINE_HELMET = 0x12e4d;
const FINE_DAGGER = 0x1397e;

test("a tempered helmet the dress laid worn, held unworn after the add, is equipped", () => {
  assert.deepStrictEqual(toEquipAfterDress([{ baseId: FINE_HELMET, worn: true }], () => false), [FINE_HELMET]);
});

test("a tempered dagger the add already equipped is left as it is", () => {
  assert.deepStrictEqual(toEquipAfterDress([{ baseId: FINE_DAGGER, worn: true }], (id) => id === FINE_DAGGER), []);
});

test("a row the record does not name worn is never equipped", () => {
  assert.deepStrictEqual(toEquipAfterDress([{ baseId: FINE_HELMET }], () => false), []);
});

test("a helmet and a dagger dressed together: only the unworn helmet is equipped, once", () => {
  const rows = [{ baseId: FINE_DAGGER, worn: true }, { baseId: FINE_HELMET, worn: true }, { baseId: FINE_HELMET, worn: true }];
  assert.deepStrictEqual(toEquipAfterDress(rows, (id) => id === FINE_DAGGER), [FINE_HELMET]);
});
