// What a captured native call crosses as, from the platform's scriptCalls
// batch: an action, a creation and a forward are sent as ScriptCall 75. A
// forward is a state call this seat's engine skipped on a subject another
// seat owns, made by a stack only this seat runs (a dialogue, scene or package
// fragment, another player's relayed entry or press); it is marked `forward`,
// and the server sends it to the subject's owner alone, who runs it there.
// Every other kind stays on this seat: a state row's change travels as its
// subject's change form, a touch is a record read, a look a trigger's own.

export type CallKind = "action" | "state" | "everyone" | "touch" | "look" | "created" | "forward";

// whether a call of this kind is sent as ScriptCall 75
export const isSent = (kind: CallKind): boolean => kind === "action" || kind === "created" || kind === "forward";

// the message's forward mark: present, as true, on a forward alone
export const forwardMark = (kind: CallKind): { forward?: true } => (kind === "forward" ? { forward: true } : {});

// whether a forward is sent: one from a quest stage's fragment (rootQuest)
// is kept only when this seat does not relay that quest's stages, since a
// relayed stage's fragment runs on the subject's owner too and makes the call
// there itself; a forward from any other root is always sent
export const keepsForward = (rootQuest: number | undefined, relays: (quest: number) => boolean): boolean =>
  rootQuest === undefined || rootQuest === 0 || !relays(rootQuest);
