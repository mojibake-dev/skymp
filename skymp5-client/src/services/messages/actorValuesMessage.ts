import { MsgType } from "../../messages";

// A player's actor values and progress, the whole snapshot (thuum
// docs/verbs/actor-values.md). The client sends it after a skill or level
// increase; the server sends its record after a login and after a value the
// server set.
export interface ActorValuesMessage {
    t: MsgType.ActorValues;
    bases: ActorValueBase[];
    skills: SkillProgress[];
    xp: number; // the character's experience toward the next level
    threshold: number; // experience the next level needs
    level: number;
    legendary: LegendarySkill[];
}

export interface ActorValueBase {
    av: number; // 0 to 163, the engine's ActorValue
    base: number;
}

export interface SkillProgress {
    skill: number; // 0 to 17
    level: number;
    xp: number;
    threshold: number;
}

export interface LegendarySkill {
    skill: number;
    count: number;
}
