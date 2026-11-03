import { MsgType } from "../../messages";
import { ChangeFormRemapRow } from "./changeFormMessage";

// thuum docs/verbs/change-form.md: a recorded change form served to a seat
// with the reference before its first frame there (with the actor's
// CreateActor word, at a cell's load) and after an owner's record is acked;
// the remap rows are the sender's, untranslated: the receiver re-encodes them
// through the engine's own id map (the native applyChangeForm's REMAP step)
export interface ChangeFormApplyMessage {
    t: MsgType.ChangeFormApply;
    formId: number;
    formType: number;
    flags: number;
    version: number;
    body: string;      // the record's bytes as hex
    remap: ChangeFormRemapRow[];
}
