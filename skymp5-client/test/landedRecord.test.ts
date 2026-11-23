// A body's place after a grip's or a stream's hold on it ends (src/sync/bodyPlace.ts recordAfterHand), JS state
// alone: the other player gripped, thrown and gripped again in quick succession stays where each throw landed.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { recordAfterHand } from "../src/sync/bodyPlace";

test("a thrown figure whose hold just ended keeps the place the throw left it while its record is the one from before the hold", () => {
  const end = recordAfterHand({ placedBefore: true, placedNow: false, heldAt: undefined, changes: 7 });
  assert.deepEqual(end, { holds: true, heldAt: 7 });
  const later = recordAfterHand({ placedBefore: false, placedNow: false, heldAt: end.heldAt, changes: 7 });
  assert.deepEqual(later, { holds: true, heldAt: 7 });
});

test("the figure's own game's next record moves it again", () => {
  assert.deepEqual(recordAfterHand({ placedBefore: false, placedNow: false, heldAt: 7, changes: 8 }), { holds: false, heldAt: undefined });
});

test("a figure no hold placed takes its record as before", () => {
  assert.deepEqual(recordAfterHand({ placedBefore: false, placedNow: false, heldAt: undefined, changes: 3 }), { holds: false, heldAt: undefined });
});

test("while a hold places the figure the hold decides, and nothing is kept for after", () => {
  assert.deepEqual(recordAfterHand({ placedBefore: true, placedNow: true, heldAt: undefined, changes: 7 }), { holds: false, heldAt: undefined });
});

test("grip, throw, grip again before any new record: each throw's landing is kept, the second at the same old record", () => {
  let heldAt: number | undefined;
  let placed = false;
  const frame = (placedNow: boolean, changes: number) => {
    const r = recordAfterHand({ placedBefore: placed, placedNow, heldAt, changes });
    placed = placedNow;
    heldAt = r.heldAt;
    return r.holds;
  };
  frame(true, 7);
  assert.strictEqual(frame(false, 7), true);
  frame(true, 7);
  assert.strictEqual(frame(false, 7), true);
  assert.strictEqual(frame(false, 9), false);
});
