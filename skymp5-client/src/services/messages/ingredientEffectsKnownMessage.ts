import { MsgType } from "../../messages";

// After the player ate an ingredient, the effects its engine now knows of it
// (thuum docs/verbs/learned-effects.md). The server keeps it only after an
// eat it saw, and teaches the player its effects again after every login.
export interface IngredientEffectsKnownMessage {
    t: MsgType.IngredientEffectsKnown;
    ingredient: number; // the INGR's form id
    mask: number; // known effects, bits 0 to 3
}
