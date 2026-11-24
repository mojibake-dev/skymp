import { MsgType } from "../../messages";

// one argument as the wire carries it (skymp-wire wire-schema shape.rs
// SnippetArg, untagged JSON): a Papyrus Bool, an Int or a Float as a number,
// a String, a form as its id and Papyrus type name, null for None. The
// handlers below read `arguments` untyped, as they always have; a message
// this game builds (ScriptCall's args) is typed by it
export type SpSnippetArgument = boolean | number | string | { formId: number; type: string } | null;

export interface SpSnippetMessage {
    t: MsgType.SpSnippet;
    class: string;
    function: string;
    arguments: any[];
    selfId: number;
    snippetIdx: number;
    // thuum docs/DECISIONS.md ADR-068 decision 4 (rung 2): the server's
    // per-object sequence on a script call it relays from the subject's owner
    // (ScriptCall 75); absent on every other snippet. A receiver applies one
    // subject's relayed calls in seq order and drops one at or below the last
    // it applied (SpSnippetService)
    seq?: number;
    // thuum #52 (wire-schema SpSnippet's `keep`, SCHEMA_VERSION 53): on a
    // relayed call the server keeps as its actor's look, the row's keep code
    // (1 EffectShader.Play, 2 its Stop, 3 VisualEffect.Play, 4 its Stop, 5
    // Actor.SetEyeTexture); absent on every other snippet, and only with seq.
    // ObjectAnimationService's 3D keep takes a look by it
    keep?: number;
    // since SCHEMA_VERSION 68 (the one script channel): on a relayed call the
    // player who acted (0x14 for this game's own), whose act the receiver
    // runs it as; a question about this game's own player (selfId 0x14),
    // which it watches and answers with a ScriptCall's answer; another
    // player's game's answer to a question this game asked about that player
    // (selfId that player's actor)
    acting?: number;
    question?: boolean;
    answer?: SpSnippetArgument;
};
