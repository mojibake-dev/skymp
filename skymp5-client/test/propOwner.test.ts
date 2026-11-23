// Who simulates a prop (src/sync/propOwner.ts), the server's owner word read
// on a seat. Ids: this seat's player 0xff000000 (hosting the cell), the other
// player 0xff000001.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { simulatorOf } from "../src/sync/propOwner";

const ME = 0xff000000;
const OTHER = 0xff000001;

test("another player's body at its rest's word (nobody) is that player's, never the cell's host's", () => {
  // the host's seat after the other player's thrown body came to rest
  assert.equal(simulatorOf({ body: OTHER, word: 0, myId: ME, hostsCell: true }), OTHER);
  assert.equal(simulatorOf({ body: OTHER, word: undefined, myId: ME, hostsCell: true }), OTHER);
});

test("this seat's own player's body at its rest's word is this seat's, on the cell's host or not", () => {
  assert.equal(simulatorOf({ body: OTHER, word: 0, myId: OTHER, hostsCell: false }), OTHER);
  assert.equal(simulatorOf({ body: ME, word: 0, myId: ME, hostsCell: true }), ME);
});

test("a word naming a player wins for a player's body: the thrown player's own stream, a gripper's hold", () => {
  assert.equal(simulatorOf({ body: OTHER, word: OTHER, myId: ME, hostsCell: true }), OTHER);
  assert.equal(simulatorOf({ body: OTHER, word: ME, myId: ME, hostsCell: true }), ME);
  assert.equal(simulatorOf({ body: OTHER, word: ME, myId: OTHER, hostsCell: false }), ME);
});

test("any other prop at a word naming nobody is the cell's host's: this seat when it hosts, else nobody here", () => {
  assert.equal(simulatorOf({ body: 0, word: 0, myId: ME, hostsCell: true }), ME);
  assert.equal(simulatorOf({ body: 0, word: undefined, myId: OTHER, hostsCell: false }), 0);
  assert.equal(simulatorOf({ body: 0, word: OTHER, myId: ME, hostsCell: true }), OTHER);
});
