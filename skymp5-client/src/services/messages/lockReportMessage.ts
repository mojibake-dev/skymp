import { MsgType } from "../../messages";

// thuum docs/verbs/locks.md (M4): how this game's lockpicking attempt on a
// locked reference ended. Client to server, reliable, R1: the engine ran the
// minigame; the server judges the report (the reference locked and within
// the activation reach, the lock's tier open to picks, a Lockpick held, a gap
// between reports) and writes the unlock or takes the broken picks from the
// record. outcome: 0 the lock turned, 1 picks broke and the lock stands, 2
// the menu was left with the lock standing; broken: picks broken, 0 to 8.
// thuum ADR-073 step 3 (decision 12): outcome 3, the engine wrote the lock by
// itself (a script's Lock or SetLockLevel, an owner's door walk, an NPC's key
// or pick) on a reference this game hosts; `locked` and `level` carry the
// lock as this engine holds it after the write, R0 the server's record of the
// owner's write (it refuses a seat that hosts neither side of the door).
export interface LockReportMessage {
    t: MsgType.LockReport;
    refr: number;
    outcome: number;
    broken: number;
    locked?: boolean;
    level?: number;
}
