// Whether the host runs an actor's AI while another seat owns its body, and
// where the promote after that hold places it (src/sync/hostPuppet.ts).
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { placesAtPromote, runsActorHere } from "../src/sync/hostPuppet";

test("the host's copy of an actor whose body a guest's grip or throw owns runs no AI: it is held as a puppet", () => {
  assert.equal(runsActorHere({ hostedHere: true, bodyOwnedElsewhere: true }), false);
});

test("the host runs its actor's AI again once no other seat's stream owns the body, and a guest never does", () => {
  assert.equal(runsActorHere({ hostedHere: true, bodyOwnedElsewhere: false }), true);
  assert.equal(runsActorHere({ hostedHere: false, bodyOwnedElsewhere: false }), false);
  assert.equal(runsActorHere({ hostedHere: false, bodyOwnedElsewhere: true }), false);
});

test("the promote after a hold for another seat's stream places nothing: the follow's end put the copy at the owner's rest", () => {
  assert.equal(placesAtPromote({ heldForBody: true, hasSample: true, seated: false }), false);
});

test("any other promote places the actor at the record's last sample, unless it is seated or has none", () => {
  assert.equal(placesAtPromote({ heldForBody: false, hasSample: true, seated: false }), true);
  assert.equal(placesAtPromote({ heldForBody: false, hasSample: true, seated: true }), false);
  assert.equal(placesAtPromote({ heldForBody: false, hasSample: false, seated: false }), false);
});
