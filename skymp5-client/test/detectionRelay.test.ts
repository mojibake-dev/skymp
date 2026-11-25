// A hosted actor's detection of each player (src/sync/detectionRelay.ts), JS state alone: what the host sends and what
// a guest takes for its own player, so a guest's theft in full view of an NPC the host's game runs finds a witness.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { detectsToSend, levelFor, UNDETECTED } from "../src/sync/detectionRelay";

const host = 0xff000000;
const guest = 0xff000001;

test("the host's NPC that sees the guest sends its level of the guest's figure", () => {
  assert.deepStrictEqual(detectsToSend([{ player: guest, level: 57 }], undefined), [{ player: guest, level: 57 }]);
});

test("a level drifting within a step sends nothing new; into sight, out of it, or a step's move sends", () => {
  const last = [{ player: guest, level: 57 }];
  assert.strictEqual(detectsToSend([{ player: guest, level: 62 }], last), undefined);
  assert.deepStrictEqual(detectsToSend([{ player: guest, level: 70 }], last), [{ player: guest, level: 70 }]);
  // a level of 3 is seen (above 0): a guest's theft then has a witness, so it always goes out
  assert.deepStrictEqual(detectsToSend([{ player: guest, level: 3 }], [{ player: guest, level: -5 }]), [{ player: guest, level: 3 }]);
  assert.deepStrictEqual(detectsToSend([{ player: guest, level: -2 }], [{ player: guest, level: 4 }]), [{ player: guest, level: -2 }]);
});

test("a player's entry that comes or goes sends, and the entries go ascending by player", () => {
  const last = [{ player: guest, level: 40 }];
  assert.deepStrictEqual(detectsToSend([{ player: guest, level: 40 }, { player: host, level: UNDETECTED }], last), [
    { player: host, level: UNDETECTED },
    { player: guest, level: 40 },
  ]);
  assert.deepStrictEqual(detectsToSend([], last), []);
});

test("the guest takes its own player's level, undetected when the host's entries leave it out, and keeps the last when a state carries none", () => {
  assert.strictEqual(levelFor([{ player: host, level: UNDETECTED }, { player: guest, level: 57 }], guest), 57);
  assert.strictEqual(levelFor([{ player: host, level: 40 }], guest), UNDETECTED);
  assert.strictEqual(levelFor(undefined, guest), undefined);
});
