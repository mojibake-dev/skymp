import { MsgType } from "../../messages";

export interface PlayerBowShotMessage {
    t: MsgType.PlayerBowShot,
    weaponId: number,
    ammoId: number,
    power: number,
    isSunGazing: boolean,
    // the aim when the arrow left, radians (thuum docs/verbs/marksman.md)
    aimAngle: number,
    aimHeading: number
};
