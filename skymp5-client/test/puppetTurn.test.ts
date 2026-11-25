// The turn a hosted actor's sample carries and what another seat's copy of
// it is fed (src/sync/puppetTurn.ts).
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { puppetTurn, sampleTurnRate } from "../src/sync/puppetTurn";

test("a hosted actor's sample carries the turn rate its engine's last movement step left", () => {
  assert.equal(sampleTurnRate({ turnRateDeg: -87.5 }), -87.5);
  assert.equal(sampleTurnRate({ turnRateDeg: 90.5 }), 90.5);
  assert.equal(sampleTurnRate({ turnRateDeg: 0 }), 0);
});

test("an actor with no movement-step sample sends no turn rate", () => {
  assert.equal(sampleTurnRate(null), undefined);
  assert.equal(sampleTurnRate({ turnRateDeg: Number.NaN }), undefined);
});

test("a copy is fed the host's turn rate and turns toward the sample's heading", () => {
  assert.deepEqual(puppetTurn({ rot: [0, 0, 203.25], turnRate: 90.5 }), { turnRateDeg: 90.5, headingDeg: 203.25 });
  assert.deepEqual(puppetTurn({ rot: [0, 0, 94], turnRate: -412 }), { turnRateDeg: -412, headingDeg: 94 });
});

test("a sample that carries no turn rate feeds the copy none, and its heading still", () => {
  assert.deepEqual(puppetTurn({ rot: [0, 0, 94] }), { turnRateDeg: 0, headingDeg: 94 });
  assert.deepEqual(puppetTurn({ rot: [0, 0, 94], turnRate: Number.NaN }), { turnRateDeg: 0, headingDeg: 94 });
});
