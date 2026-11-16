import { MsgType } from "../../messages";

// thuum docs/verbs/summon.md (tracker #84; lab/.cache/drafts/summons/design.md
// step 3): the caster's report of its own summon, one message for its start
// (ended false: the base, the effect, the spell, the place and the seconds
// left, as the caster's engine has them) and its end (ended true: the local
// id alone, zeros elsewhere; the wire reads nothing else at an end,
// wire-schema skymp.rs's Summon doc). Client to server, reliable, R2 (the
// caster's engine computes its summon's place and life; the server records
// within bounds). Naming no actor, it is only ever the sender's own player's
// summon (summonService filters the platform's summonStart and summonEnd to
// the player as the caster). rot is radians (the same doc: "which way the
// summon faces, radians"), this engine's angle reads being degrees.
export interface SummonMessage {
    t: MsgType.Summon;
    localId: number; // the summon's id in this game's engine, a runtime id 0xff000000 and up
    baseId: number; // the summon's base (an NPC_ of the load order); 0 at an end
    effectId: number; // the summon effect (an MGEF); 0 at an end
    spellId: number; // the spell as this engine holds it, 0 when the event named none; 0 at an end
    pos: number[]; // where the summon stands, this engine's coordinates; [0, 0, 0] at an end
    rot: number[]; // which way the summon faces, radians; [0, 0, 0] at an end
    worldOrCell: number; // the worldspace or cell of the place; 0 at an end
    remainingS: number; // the seconds the engine reports left, at least 0; 0 at an end
    ended: boolean; // false: the start of the summon localId names; true: its end
}
