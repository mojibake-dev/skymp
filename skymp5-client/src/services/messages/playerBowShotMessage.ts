import { MsgType } from "../../messages";

export interface PlayerBowShotMessage {
    t: MsgType.PlayerBowShot,
    // the shooter, as the server named it: the player's own idx for its own
    // shot, a hosted actor's for one this game simulates (thuum
    // docs/verbs/npc-stream.md, train 53; networkingService fills it from
    // _refrId, the player's slot when none)
    idx: number,
    weaponId: number,
    ammoId: number,
    power: number,
    isSunGazing: boolean,
    // the aim when the arrow left, radians (thuum docs/verbs/marksman.md)
    aimAngle: number,
    aimHeading: number,
    // the shooter's own launch as its engine made it (projectileLaunched):
    // origin in engine units, pitch and heading in radians; when hasLaunch is
    // false the five are zero (the arrow's path identical on both screens)
    hasLaunch: boolean,
    originX: number,
    originY: number,
    originZ: number,
    launchAngleX: number,
    launchAngleZ: number
};
