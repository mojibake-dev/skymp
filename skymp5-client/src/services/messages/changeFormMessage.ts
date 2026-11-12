import { MsgType } from "../../messages";

// thuum docs/verbs/change-form.md (ADR-065, system 1's slice; the first
// trigger ADR-067's corpse): the engine's own change form of a reference an
// owner's game serialized, client to server, reliable. The server validates
// the header against the body (the type byte, the version 0x4E, the lengths),
// the hash, the sender's ownership of the actor's cell at the epoch, the
// remap rows' decode and the caps (E_CHANGEFORM_*), records and acks it, and
// serves it as ChangeFormApply. The body rides as hex, two characters per
// byte (the fork's serializeChangeForm hands it so); the message's fields
// are the contract's alone (no idx: the form id is the shared load order's)
export interface ChangeFormRemapRow {
    offset: number;   // the id's offset inside the body
    formId: number;   // the form the seat-local id names
    index: number;    // the seat-and-session index encoded there (kind 0)
}

export interface ChangeFormMessage {
    t: MsgType.ChangeForm;
    epoch: number;     // the cell's host epoch this client holds for the actor's cell (ADR-066's stamp)
    formId: number;    // the shared load order's id (a game-file actor's own)
    formType: number;  // the record's save-type index (0 REFR, 1 ACHR), the header byte's low six bits; not the engine's form type
    flags: number;     // the change flags, u32
    version: number;   // the save version byte, 0x4E
    body: string;      // the record's bytes as hex (cap 65536 bytes)
    fnv1a: number;     // FNV-1a 32 over the body's bytes
    remap: ChangeFormRemapRow[]; // the id census (cap 256 rows)
}
