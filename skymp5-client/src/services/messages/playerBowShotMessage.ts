import { MsgType } from "../../messages";

export interface PlayerBowShotMessage {
    t: MsgType.PlayerBowShot,
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
