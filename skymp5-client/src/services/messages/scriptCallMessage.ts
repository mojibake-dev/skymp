import { MsgType } from "../../messages";
import { SpSnippetArgument } from "./spSnippetMessage";

// thuum docs/DECISIONS.md ADR-068 decision 4 (rung 2, the one seam;
// docs/research/scripts-one-system-2026-11-05.md 6.4): a script's call of a
// tabled ACTION native (docs/NATIVES.md's kind column: an animation event or
// an animation variable, which the engine never saves) on a subject this game
// owns, as the platform's hook on the VM's native dispatch read it from the
// typed frame (the scriptCalls batch, ScriptCallService). Client to server,
// reliable, R2. The server checks the SHAPE, never the value: the pair an
// action row of the table, the args' count and kinds the row's, the sender the
// subject's owner at the epoch, a per-sender budget (E_SCRIPTCALL_ROW, _ARGS,
// _OWNER, _RATE); then it mirrors the same native (a latent row's twin) to the
// subject's other listeners as SpSnippet 30 with the object's seq. Since rung
// 4 (amendment B, the state rows) the same shape carries an owned RECORD
// row's call too, RefState's five and the standing's six, which the server
// records (wire-rules ref_state.rs, standing.rs) and says back as
// DestructionStage 78 or as applier snippets (ScriptCallService's record
// queue). JSON field names as wire-schema's ScriptCall reads them
export interface ScriptCallMessage {
    t: MsgType.ScriptCall;
    epoch: number;   // the cell's host epoch this client holds for the subject's cell (ADR-066's stamp, HostService.epochForActor), 0 for none
    selfId: number;  // the subject's server id (a method's self, a static row's first argument), never 0 and never a player's figure: this game's own actor is 0x14
    cls: string;     // the Papyrus class the native is declared on, "ObjectReference"
    fn: string;      // the native's name, "PlayAnimationAndWait"
    args: SpSnippetArgument[]; // the script's arguments in SpSnippet's own shape (at most 11; a form by its server id)
    caller: string;  // the calling script, the frame below the native's ("defaultPuzzlePillarScript")
    handler: string; // the calling function in it ("OnActivate")
    latent: boolean; // whether the native is latent (the server relays a latent row as its non-latent twin)
}
