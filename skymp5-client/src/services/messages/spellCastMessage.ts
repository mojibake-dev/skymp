import { MsgType } from "../../messages";

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
}

// a cast without its launch: the wire requires the fields (a missing key is
// E_JSON_SHAPE), all zero
export const noSpellLaunch = { hasLaunch: false, originX: 0, originY: 0, originZ: 0, launchAngleX: 0, launchAngleZ: 0 };

export interface SpellCastMessage {
    t: MsgType.SpellCast,
    data: SpellCastMsgData
}
