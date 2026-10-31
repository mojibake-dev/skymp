import { MsgType } from "../../messages";

// thuum docs/verbs/leveling.md (M5): the skill uses this game's engine made in
// the last second, batched: each skill once (its actor value, 6 to 23) with
// the actions' base amounts summed. Client to server, reliable; the server
// bounds the amount and runs the game's formulas into the record.
export interface SkillUseEntry {
    skill: number;
    amount: number;
}

export interface SkillUseMessage {
    t: MsgType.SkillUse;
    uses: SkillUseEntry[];
}
