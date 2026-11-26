// Whether an NPC another game runs fights this seat's side, read from its
// host's combat target (src/sync/sideTarget.ts): PuppetService puts its copy
// here into combat with this seat's player for this seat's own player and
// for this seat's own minions alike, since a player and its summons and
// thralls are one side.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { fightsThisSide } from "../src/sync/sideTarget";

// this seat's player and its own summon and thrall, by server id
const ME = 0xff000d21;
const SUMMON = 0xff000e01;
const THRALL = 0x85547;
const OWN = new Set([SUMMON, THRALL]);

test("an NPC whose host's target is this seat's player fights this seat's side", () => {
  assert.equal(fightsThisSide(ME, ME, OWN), true);
});

test("an NPC whose host's target is one of this seat's own summons or thralls fights this seat's side", () => {
  assert.equal(fightsThisSide(SUMMON, ME, OWN), true);
  assert.equal(fightsThisSide(THRALL, ME, OWN), true);
});

test("another player, another player's minion, or no target is not this seat's side", () => {
  assert.equal(fightsThisSide(0xff000d20, ME, OWN), false);
  assert.equal(fightsThisSide(0xff000e02, ME, OWN), false);
  assert.equal(fightsThisSide(0, ME, OWN), false);
});

test("with no server id of its own player yet, this seat has no side to fight", () => {
  assert.equal(fightsThisSide(SUMMON, 0, OWN), false);
  assert.equal(fightsThisSide(0, 0, OWN), false);
});
