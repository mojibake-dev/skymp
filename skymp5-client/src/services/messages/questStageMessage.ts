import { MsgType } from "../../messages";

// The world's quest stages (thuum docs/verbs/quest-stage.md, ADR-056; memo
// D58 B). To the server: one stage this game set on a shared quest (quest,
// stage; no list, no mode). From the server: at a login the sharing mode and
// its list (`mode` 0 list or 1 all; `shared` the quests to report in mode 0,
// the DENIED quests never to report in mode 1; with quest 0 and stage 0),
// then the world's recorded stages one by one, and later the relay of
// another seat's stage. A stage already done here is nothing.
export interface QuestStageMessage {
    t: MsgType.QuestStage;
    quest: number;
    stage: number;
    shared?: number[];
    mode?: number;
}
