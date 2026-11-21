import { MsgType } from "../../messages";
import { CreatedForm } from "../../sync/createdForms";

export interface SpellCastMsgData {
    caster: number
    target: number
    spell: number
    isDualCasting: boolean
    interruptCast: boolean
    castingSource: number
    aimAngle: number,
    aimHeading: number,
    // the caster's own launch as its engine made it (projectileLaunched):
    // for a target-location spell, the point its game picked along the aim,
    // which each neighbour's game launches the spell at; origin in engine
    // units, pitch and heading in radians; when hasLaunch is false the five
    // are zero (thuum docs/verbs/spell-cast.md)
    hasLaunch: boolean,
    originX: number,
    originY: number,
    originZ: number,
    launchAngleX: number,
    launchAngleZ: number,
    actorAnimationVariables: {
        booleans: number[]
        floats: number[]
        integers: number[]
    }
    // thuum docs/verbs/spell-cast.md (the rune's glyph on the observer): the
    // surface the caster's own target-location launch picked, read in its
    // game from the engine's decal placement (the fork's runeDecal event):
    // the pick's unit normal and the surface's material id, which each
    // observer's game hands to the placement after its relayed launch
    // (castSpellImmediate's last five arguments). Only with hasLaunch;
    // absent for every other cast
    decal?: SpellCastDecal
    // thuum tracker #98 (SCHEMA_VERSION 63, Eli's look 2: "other player does
    // not see fire"): the server's relay of an enchanted weapon's hit names a
    // made enchantment by its definition, with spell 0; a seat's own cast never
    // carries one
    createdSpell?: CreatedForm
}

export interface SpellCastDecal {
    normalX: number
    normalY: number
    normalZ: number
    materialId: number
}

// a cast without its launch: the wire requires the fields (a missing key is
// E_JSON_SHAPE), all zero
export const noSpellLaunch = { hasLaunch: false, originX: 0, originY: 0, originZ: 0, launchAngleX: 0, launchAngleZ: 0 };

export interface SpellCastMessage {
    t: MsgType.SpellCast,
    data: SpellCastMsgData
}
