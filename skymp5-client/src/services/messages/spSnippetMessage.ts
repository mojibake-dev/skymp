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
};
