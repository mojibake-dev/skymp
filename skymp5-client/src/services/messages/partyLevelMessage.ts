import { MsgType } from "../../messages";

// The level this game's PC-level-mult NPCs scale to: the party's level of
// the players this game stands with (the mean of their levels leaned toward
// the highest by the server setting npcScaleLean, cut to a whole level),
// sent when it changes and at a login. This game's engine computes a
// PC-level-mult NPC's level and stats from it in place of its own player's
// level, so the engine and the server count the same health. Server to
// client, reliable, R0. The contract is skymp-wire's (wire-schema skymp.rs,
// PartyLevel, MsgType 90).
export interface PartyLevelMessage {
    t: MsgType.PartyLevel;
    level: number; // the party's level, at least 1
}
