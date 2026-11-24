// The script channel's messages on this seat, JS state alone.
// Sending: a batched call becomes ScriptCall 75. A forward (a state call this
// seat's engine skipped on a subject another seat owns) names what started it
// and the player who acted, when the platform read them. A question (a look
// only another player's own game can make: Actor.HasLOS on that player's
// figure inside its acting scope) is sent with self 0x14, epoch 0, forward and
// the acting player, whom the server asks; one with no acting player is never
// sent (the wire's validator refuses it).
// Receiving: an SpSnippet with `question` asks this game about its own player
// (self 0x14): it starts a look watch (lookWatch.ts) and is never run. One
// with `answer` is another player's game's answer to a question this seat
// asked: its look is written for this seat's scripts and nothing is run.
// Every other snippet runs as before; a relayed one's `acting` names the
// player whose act it is (0x14 for this game's own).

import { MsgType } from "../messages";
import { ScriptCallMessage } from "../services/messages/scriptCallMessage";
import { SpSnippetArgument, SpSnippetMessage } from "../services/messages/spSnippetMessage";
import { CallKind, forwardMark } from "./scriptForward";

// a batched call's fields this module reads (the platform's ScriptCall)
export interface ChannelCall {
  kind: CallKind;
  cls: string;
  fn: string;
  caller: string;
  handler: string;
  latent: boolean;
  landed?: { formId: number; count: number }[];
  cause?: { kind: number; form: number; n: number };
  acting?: number;
}

export type SnippetRole = "question" | "answer" | "run";

// the look a question reads and its answer carries
const LOOK_CLASS = "Actor";
const LOOK_FN = "HasLOS";

// a snippet argument's form id, 0 for None or an argument that is no object
const formIdOf = (arg: unknown): number => {
  const formId = arg !== null && typeof arg === "object" ? (arg as { formId?: unknown }).formId : undefined;
  return typeof formId === "number" && Number.isInteger(formId) && formId > 0 ? formId : 0;
};

// the ScriptCall a batched call is sent as, its self and epoch as the sender
// mapped them and its arguments packed; undefined for a question with no
// acting player
export const callMessage = (call: ChannelCall, selfId: number, epoch: number, args: SpSnippetArgument[]): ScriptCallMessage | undefined => {
  const question = call.kind === "question";
  if (question && typeof call.acting !== "number") {
    return undefined;
  }
  const message: ScriptCallMessage = {
    t: MsgType.ScriptCall,
    epoch: question ? 0 : epoch,
    selfId: question ? 0x14 : selfId,
    cls: call.cls,
    fn: call.fn,
    args,
    caller: call.caller,
    handler: call.handler,
    latent: call.latent,
    ...(question ? { forward: true } : forwardMark(call.kind)),
  };
  if (call.landed !== undefined) {
    message.landed = call.landed.map((landing) => ({ item: landing.formId, count: landing.count }));
  }
  if (call.kind === "forward" && call.cause !== undefined) {
    message.cause = { kind: call.cause.kind, form: call.cause.form, n: call.cause.n };
  }
  if ((question || call.kind === "forward") && typeof call.acting === "number") {
    message.acting = call.acting;
  }
  return message;
};

// this game's answer to a question about its own player: whether it sees the
// target, the target as the question named it
export const answerMessage = (target: number, los: boolean): ScriptCallMessage => ({
  t: MsgType.ScriptCall,
  epoch: 0,
  selfId: 0x14,
  cls: LOOK_CLASS,
  fn: LOOK_FN,
  args: [{ formId: target, type: "ObjectReference" }],
  caller: "",
  handler: "",
  latent: false,
  answer: los,
});

export const snippetRole = (msg: Pick<SpSnippetMessage, "question" | "answer">): SnippetRole =>
  msg.question === true ? "question" : msg.answer !== undefined ? "answer" : "run";

// a question's target as the question names it, 0 when it is no question
// about this game's own player or names no target
export const lookQuestion = (msg: SpSnippetMessage): number =>
  msg.question === true && msg.selfId === 0x14 ? formIdOf(msg.arguments[0]) : 0;

// an answer's look: the answering player by its server id, the target as the
// answer names it and whether the player sees it; undefined for an answer
// with no target
export const lookAnswer = (msg: SpSnippetMessage): { player: number; target: number; los: boolean } | undefined => {
  const target = formIdOf(msg.arguments[0]);
  if (msg.answer === undefined || target === 0) {
    return undefined;
  }
  return { player: msg.selfId, target, los: msg.answer === true };
};

// a relayed call's line naming the player whose act it runs as; undefined for
// a snippet with no acting player
export const actingLine = (msg: SpSnippetMessage): string | undefined => {
  if (typeof msg.acting !== "number") {
    return undefined;
  }
  const seq = typeof msg.seq === "number" ? ` (seq ${msg.seq})` : "";
  const who = msg.acting === 0x14 ? "this game's own player's" : `${msg.acting.toString(16)}'s`;
  return `relayed ${msg.class}.${msg.function} on ${msg.selfId.toString(16)}${seq} runs as ${who} act`;
};
