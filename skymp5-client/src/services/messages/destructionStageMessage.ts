import { MsgType } from "../../messages";

// thuum docs/DECISIONS.md ADR-068 D-S3 point 3 (traps follow the interaction;
// docs/research/trap-slice-d-s3-2026-11-05.md): a single-use result recorded
// once. Both directions, reliable. To the server: this seat's report of a trap
// family or destructible reference's destruction stage rise, or its
// destruction, that this seat's engine caused or saw (served false;
// TrapResultService); the server keeps the first report of each step (the
// stage only rises, destroyed only goes true) on the reference's record beside
// its lock. From the server: a newly recorded step relayed to the reference's
// other listeners (served false), and the record served to a late subscriber
// right after its create word and after a restart (served true), which this
// seat applies under the platform's apply blank (setRecordApplying). The
// validator: refr not 0 and below 0xff000000, stage at most 127, a burst of 8
// then 2 a second. JSON field names as wire-schema's DestructionStage reads
// them.
// ADR-068 amendment B (rung 4, the state rows; docs/research/
// rung4-state-rows-2026-11-07.md (b)): the one S2C word for a reference's
// state. The server's word may carry the reference's engine state beside its
// destruction, its disabled and deleted flags as the record holds them, with
// the object's seq (the per-object sequence ScriptCall's relays share): a
// host's Enable, Disable or Delete relayed to the reference's other
// listeners (served false), and the record served behind its create word
// (served true). RefStateService applies that half; this seat's report
// carries neither flag and seq 0, which the validator holds it to
// (E_VAL_RANGE)
export interface DestructionStageMessage {
    t: MsgType.DestructionStage;
    refr: number;       // the reference's form id, a game-file reference's (never 0, below 0xff000000)
    stage: number;      // its destruction stage, 0 to 127 (the engine's GetCurrentDestructionStage)
    destroyed: boolean; // its destroyed flag (SetDestroyed, or a stage that destroys it)
    served: boolean;    // false for a live report or relay, true for the record served (a late subscriber, a restart)
    disabled?: boolean; // from the server alone: the reference's disabled flag as the record holds it; absent when the word carries no ref state
    deleted?: boolean;  // from the server alone: the reference is deleted (a game-file reference stays disabled); absent likewise
    seq: number;        // the object's seq at the word (the server's per-object sequence); 0 on a report and on a destruction step
}
