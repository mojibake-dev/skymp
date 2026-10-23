import { MsgType } from "../../messages";

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the server's
// roll for the game-file actors of the cells this game is about to attach,
// sent ahead of the engine's own roll at the reference's load and again at a
// respawn: the NPC_ the roll chose (0 for none), the level it used and the
// worn items. The fate service pins the engine's roll from it; a fate that
// arrives after the engine rolled goes through the engine's own re-roll.
export interface ActorFateEntry {
    refrId: number;
    baseId: number;
    level: number;
    worn: number[];
}

export interface ActorFateMessage {
    t: MsgType.ActorFate;
    fates: ActorFateEntry[];
}
