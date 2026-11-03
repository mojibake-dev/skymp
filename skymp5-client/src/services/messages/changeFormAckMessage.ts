import { MsgType } from "../../messages";

// thuum docs/verbs/change-form.md: the server took a ChangeForm; the hash it
// holds is the owner's compare baseline (equal bytes send nothing)
export interface ChangeFormAckMessage {
    t: MsgType.ChangeFormAck;
    formId: number;
    fnv1a: number;
}
