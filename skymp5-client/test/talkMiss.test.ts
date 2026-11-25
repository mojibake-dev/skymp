// What an E press found when it found no actor to talk to
// (src/sync/talkMiss.ts), JS state alone.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { missLine } from "../src/sync/talkMiss";

test("a guest's E at a gate guard with nothing under the crosshair says the crosshair held nothing", () => {
  assert.strictEqual(missLine(null), "E: nothing under the crosshair");
});

test("a guest's E at a seated gate guard that the crosshair finds the guard's seat for names the seat, its base and how far it is", () => {
  // a reference (REFR 0x3D, CommonLibSSE-NG include/RE/F/FormTypes.h:201) whose base is a furniture
  // (FURN 0x28, FormTypes.h:180)
  const line = missLine({ formId: 0xf50d5, formType: 61, baseId: 0x1f2a7, baseType: 40, distance: 168.4 });
  assert.strictEqual(
    line,
    "E: no actor under the crosshair: f50d5 (form type 61), base 1f2a7 (form type 40), 168 units from the player",
  );
});

test("a held reference with no base names base 0", () => {
  const line = missLine({ formId: 0xff000abc, formType: 61, baseId: 0, baseType: 0, distance: 95 });
  assert.strictEqual(line, "E: no actor under the crosshair: ff000abc (form type 61), base 0 (form type 0), 95 units from the player");
});
