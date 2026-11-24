// A quest's scenes run on the game of its acting player (src/sync/questActing.ts, claimRules.ts's SceneMarks), JS
// state alone. The guest walks alone into Bleak Falls' pillar room while the host's player is elsewhere in the
// Temple: the host's game runs the trigger's script as the guest's act and starts the lever quest, whose lever scene
// begins with the quest and measures its victim's distance to "the player". That scene is the guest's game's to run
// and claim, and the host's game claims nothing for it.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { QuestActing, relayScope, sceneOwnHere } from "../src/sync/questActing";
import { SceneMarks } from "../src/services/services/claimRules";

const LEVER_QUEST = 0x3ac44;
const LEVER_SCENE = 0x3ac45;
const VICTIM = 0x39fe1;
const GUEST = 0xff000001;

test("on the host: its own report of the start the guest's entry made names the guest, so the lever scene is not the host's", () => {
  const acting = new QuestActing();
  acting.noteReport(LEVER_QUEST, GUEST, false);
  assert.strictEqual(acting.seatOf(LEVER_QUEST), "other");
  assert.strictEqual(acting.actingOf(LEVER_QUEST), GUEST);
  assert.strictEqual(sceneOwnHere(acting.seatOf(LEVER_QUEST), "start"), false);
  assert.strictEqual(sceneOwnHere(acting.seatOf(LEVER_QUEST), "action-mine"), false);
});

test("on the guest: the relayed start stamped with its own player arms no relayed scope, and the scene is its own even on a puppet", () => {
  const acting = new QuestActing();
  acting.noteRelay(LEVER_QUEST, GUEST, true);
  assert.strictEqual(acting.seatOf(LEVER_QUEST), "here");
  assert.strictEqual(relayScope(GUEST, true), "none");
  assert.strictEqual(sceneOwnHere(acting.seatOf(LEVER_QUEST), "start"), true);
  assert.strictEqual(sceneOwnHere(acting.seatOf(LEVER_QUEST), "action-puppet"), true);
});

test("this seat's own report with no acting player is this seat's", () => {
  const acting = new QuestActing();
  acting.noteReport(LEVER_QUEST, undefined, false);
  assert.strictEqual(acting.seatOf(LEVER_QUEST), "here");
});

test("a relay stamped with another player arms that player's acting scope; an unstamped one a relayed scope, its seat unknown", () => {
  assert.strictEqual(relayScope(GUEST, false), "acting");
  assert.strictEqual(relayScope(undefined, false), "relayed");
  const acting = new QuestActing();
  acting.noteRelay(LEVER_QUEST, GUEST, true);
  acting.noteRelay(LEVER_QUEST, undefined, false);
  assert.strictEqual(acting.seatOf(LEVER_QUEST), "unknown");
});

test("the latest start or stage decides", () => {
  const acting = new QuestActing();
  acting.noteReport(LEVER_QUEST, undefined, false);
  acting.noteRelay(LEVER_QUEST, GUEST, false);
  assert.strictEqual(acting.seatOf(LEVER_QUEST), "other");
  acting.noteRelay(LEVER_QUEST, GUEST, true);
  assert.strictEqual(acting.seatOf(LEVER_QUEST), "here");
});

test("a quest no start or stage named keeps the rules as they were", () => {
  const seat = new QuestActing().seatOf(LEVER_QUEST);
  assert.strictEqual(seat, "unknown");
  assert.strictEqual(sceneOwnHere(seat, "start"), true);
  assert.strictEqual(sceneOwnHere(seat, "start-acting"), false);
  assert.strictEqual(sceneOwnHere(seat, "action-mine"), true);
  assert.strictEqual(sceneOwnHere(seat, "action-puppet"), undefined);
});

test("the table is bounded: past its cap the quest noted longest ago goes", () => {
  const acting = new QuestActing(2);
  acting.noteReport(1, undefined, false);
  acting.noteReport(2, undefined, false);
  acting.noteReport(3, undefined, false);
  assert.deepEqual([acting.seatOf(1), acting.seatOf(2), acting.seatOf(3)], ["unknown", "here", "here"]);
});

test("on the host: a scene its start marked its own is ceded once its quest's seat reads the guest's, and its claims go back", () => {
  const marks = new SceneMarks();
  marks.mark(LEVER_SCENE, 0, "Scene.Start", true);
  marks.noteClaim({ localId: VICTIM, remoteId: VICTIM, sceneId: LEVER_SCENE, quest: LEVER_QUEST, at: 10, mine: true });
  const released = marks.cede(LEVER_SCENE, "its quest's acting player is ff000001");
  assert.deepEqual(released?.map((c) => c.remoteId), [VICTIM]);
  assert.strictEqual(marks.isOwn(LEVER_SCENE), false);
  assert.strictEqual(marks.claims(LEVER_SCENE), false);
  assert.strictEqual(marks.holdsStop(VICTIM, LEVER_SCENE, 20), false);
  assert.strictEqual(marks.cede(LEVER_SCENE, "again"), null);
});
