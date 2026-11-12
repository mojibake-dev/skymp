import { MsgType } from "../../messages";

// The world's quest stages (thuum docs/verbs/quest-stage.md, ADR-056; memo
// D58 B). To the server: one stage this game set on a shared quest (quest,
// stage; no list, no mode). From the server: at a login the sharing mode and
// its list (`mode` 0 list or 1 all; `shared` the quests to report in mode 0,
// the DENIED quests never to report in mode 1; with quest 0 and stage 0),
// then the world's recorded stages one by one, and later the relay of
// another seat's stage. A stage already done here is nothing.
// ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md 1(a)): a quest's
// run state crosses as a word on the same message (`word` 1 a start, 2 a
// stop, its stage 0); the server stamps the acting player on a relay
// (`acting`), and a seat names the scope's player when its engine set the
// stage or ran the word inside one (absent: the sender itself); `served`
// marks a stage or word to apply as a load (the login's replay; D-Q3 A),
// never true from a client. An absent field is no key on the wire (the
// validator: a word 1 or 2 with stage 0 and a quest, acting at 0xff000000
// and up). JSON field names as wire-schema's QuestStage reads them.
// D-Q4 C (SCHEMA_VERSION 43): a seat's report names what made it, `cause` 1
// a world cause or 2 the seat's own, and `root`, the root frame of the script
// stack that made it ("<Script>.<Function>", absent when no script call did);
// both the seat's own word (R2), the server judging a report it reads as a
// start by them
export interface QuestStageMessage {
    t: MsgType.QuestStage;
    quest: number;
    stage: number;
    shared?: number[];
    mode?: number;
    player?: number[]; // on the login's message (quest 0) alone: the player-class quests, in `shared`'s entry shape; each seat runs its own player's (setPlayerQuests)
    word?: number;     // QUEST_START 1 or QUEST_STOP 2; absent for a stage
    wordStages?: number[]; // on a word from the server: the quest's StartUpStage(s) for a start, its ShutDownStage(s) for a stop (u16s)
    acting?: number;   // the acting player's actor (a server id, 0xff000000 and up)
    served?: boolean;  // from the server alone: apply as a load, its fragment skipped
    cause?: number;    // a seat's report alone: 1 a world cause, 2 the seat's own (D-Q4 C)
    root?: string;     // beside `cause`: "<Script>.<Function>", at most 257 bytes of [A-Za-z0-9_.]
}
