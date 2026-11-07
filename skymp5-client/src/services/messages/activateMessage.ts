import { MsgType } from "../../messages";

export interface ActivateMessage {
    t: MsgType.Activate,
    data: ActivateMessageData
}

interface ActivateMessageData {
    caster: number;
    target: number;
    isSecondActivation: boolean;
    // thuum docs/DECISIONS.md D-C2 A (docs/research/fifty-fixes-2026-11-07.md
    // 1(a)): 1 OnTriggerEnter, 2 OnTriggerLeave, the presser's owner's report
    // of an entry into a trigger whose world script runs on the cell's host;
    // caster the presser (0x14 for this seat's own player, or an actor it
    // simulates), target the trigger, isSecondActivation false. Absent on
    // every activation
    entry?: number;
}
