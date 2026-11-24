// The script channel's client half (src/sync/scriptChannel.ts), JS state alone. The host's run of a word wall's
// script asks whether the guest sees the wall's look marker: the host sends a question, the guest's own game watches
// its look and answers, and the host writes the answer where its script reads it. A forward names what started it
// and the player who acted.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { MsgType } from "../src/messages";
import { SpSnippetMessage } from "../src/services/messages/spSnippetMessage";
import { ChannelCall, actingLine, answerMessage, callMessage, lookAnswer, lookQuestion, snippetRole } from "../src/sync/scriptChannel";
import { isSent } from "../src/sync/scriptForward";
import { LookWatches } from "../src/sync/lookWatch";

const MARKER = 0x709d4;
const GUEST = 0xff000a12;
const MARKER_ARG = { formId: MARKER, type: "ObjectReference" };

const call = (over: Partial<ChannelCall>): ChannelCall => ({
  kind: "action", cls: "Actor", fn: "HasLOS", caller: "WordWallTriggerScript", handler: "OnUpdate", latent: false, ...over,
});

const snippet = (over: Partial<SpSnippetMessage>): SpSnippetMessage => ({
  t: MsgType.SpSnippet, class: "Actor", function: "HasLOS", arguments: [MARKER_ARG], selfId: 0x14, snippetIdx: 0xffffffff,
  ...over,
});

test("the host's look at the guest is sent as a question: self 0x14, epoch 0, forward, the acting player", () => {
  assert.strictEqual(isSent("question"), true);
  assert.deepEqual(callMessage(call({ kind: "question", acting: GUEST }), 0x5a, 7, [MARKER_ARG]), {
    t: MsgType.ScriptCall, epoch: 0, selfId: 0x14, cls: "Actor", fn: "HasLOS", args: [MARKER_ARG],
    caller: "WordWallTriggerScript", handler: "OnUpdate", latent: false, forward: true, acting: GUEST,
  });
});

test("a question with no acting player is never sent", () => {
  assert.strictEqual(callMessage(call({ kind: "question" }), 0x14, 0, [MARKER_ARG]), undefined);
});

test("a forward carries its cause and its acting player, on its own subject and epoch", () => {
  const cause = { kind: 2, form: 0x2d5c1, n: 0 };
  const m = callMessage(call({ kind: "forward", cls: "ObjectReference", fn: "Lock", cause, acting: GUEST }), 0x1a6f4, 3, [false]);
  assert.deepEqual(
    m && { selfId: m.selfId, epoch: m.epoch, forward: m.forward, cause: m.cause, acting: m.acting },
    { selfId: 0x1a6f4, epoch: 3, forward: true, cause, acting: GUEST });
});

test("a forward without a cause or an acting player sends neither, and an action carries no channel field", () => {
  const forward = callMessage(call({ kind: "forward", cls: "ObjectReference", fn: "Lock" }), 0x1a6f4, 3, [false]);
  assert.deepEqual(forward && ["cause" in forward, "acting" in forward, forward.forward], [false, false, true]);
  const action = callMessage(call({ kind: "action", cause: { kind: 5, form: 0x1a6f4, n: 0 }, acting: GUEST }), 0x1a6f4, 3, []);
  assert.deepEqual(action && ["forward" in action, "cause" in action, "acting" in action], [false, false, false]);
});

test("a leveled roll's landings still travel by item and count", () => {
  const m = callMessage(call({ kind: "action", cls: "ObjectReference", fn: "AddItem", landed: [{ formId: 0x1397e, count: 2 }] }), 0x14, 0, []);
  assert.deepEqual(m?.landed, [{ item: 0x1397e, count: 2 }]);
});

test("the guest's game takes a question about its own player as a watch, never a run: the first read and each change are answered", () => {
  const q = snippet({ question: true });
  assert.strictEqual(snippetRole(q), "question");
  assert.strictEqual(lookQuestion(q), MARKER);
  const watches = new LookWatches();
  watches.ask(lookQuestion(q), 0);
  assert.strictEqual(watches.read(MARKER, false), true);
  assert.deepEqual(answerMessage(MARKER, false), {
    t: MsgType.ScriptCall, epoch: 0, selfId: 0x14, cls: "Actor", fn: "HasLOS", args: [MARKER_ARG],
    caller: "", handler: "", latent: false, answer: false,
  });
  assert.strictEqual(watches.read(MARKER, false), false);
  assert.strictEqual(watches.read(MARKER, true), true);
  assert.strictEqual(answerMessage(MARKER, true).answer, true);
});

test("an answer never carries forward or acting", () => {
  const a = answerMessage(MARKER, true);
  assert.deepEqual(["forward" in a, "acting" in a], [false, false]);
});

test("a question about anyone but this game's own player, or with no target, starts no watch", () => {
  assert.strictEqual(lookQuestion(snippet({ question: true, selfId: GUEST })), 0);
  assert.strictEqual(lookQuestion(snippet({ question: true, arguments: [] })), 0);
  assert.strictEqual(lookQuestion(snippet({ question: true, arguments: [null] })), 0);
});

test("the guest's answer reaches the host as a look to write by the guest and the target, and runs nothing", () => {
  const a = snippet({ selfId: GUEST, answer: true });
  assert.strictEqual(snippetRole(a), "answer");
  assert.deepEqual(lookAnswer(a), { player: GUEST, target: MARKER, los: true });
  assert.deepEqual(lookAnswer(snippet({ selfId: GUEST, answer: false })), { player: GUEST, target: MARKER, los: false });
  assert.strictEqual(lookAnswer(snippet({ selfId: GUEST, answer: true, arguments: [] })), undefined);
});

test("a relayed call with neither runs as before, and its line names the player whose act it is", () => {
  const relayed = snippet({ class: "ObjectReference", function: "Activate", selfId: 0x1a6f4, seq: 12, acting: 0x14 });
  assert.strictEqual(snippetRole(relayed), "run");
  assert.match(actingLine(relayed) ?? "", /^relayed ObjectReference\.Activate on 1a6f4 \(seq 12\) runs as this game's own player's act$/);
  assert.match(actingLine({ ...relayed, acting: GUEST }) ?? "", /runs as ff000a12's act$/);
  assert.strictEqual(actingLine({ ...relayed, acting: undefined }), undefined);
});
