import { MsgType } from "../../messages";

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the server's
// roll for the game-file actors of the cells this game is about to attach,
// sent ahead of the engine's own roll at the reference's load and again at a
// respawn: the NPC_ the roll chose (0 for none), the level it used and the
// worn items. The fate service pins the engine's roll from it; a fate that
// arrives after the engine rolled goes through the engine's own re-roll.
export interface ActorFateInvRow {
    baseId: number;
    count: number;
}

export interface ActorFateEntry {
    refrId: number;
    baseId: number;
    level: number;
    worn: number[];
    // the record's inventory rows apart from the worn items (thuum
    // docs/verbs/npc-fate.md, playtest 44's 1c): the server's own container
    // roll, for the copy's inventory to carry instead of the engine's
    inv: ActorFateInvRow[];
}

// the rows as the platform's exports take them: one flat array
// [formId, count, formId, count, ...] (callNative-free Napi exports take an
// array; a nested object array would cost a parse the detour's side avoids)
export const flatInvRows = (rows: ActorFateInvRow[] | undefined): number[] => {
    const out: number[] = [];
    for (const row of rows ?? []) {
        out.push(row.baseId, row.count);
    }
    return out;
};

export interface ActorFateMessage {
    t: MsgType.ActorFate;
    fates: ActorFateEntry[];
}
