import { MsgType } from "../../messages";

// A neighbour's shot the server took, for this game to launch the arrow from
// that player's figure (thuum docs/verbs/marksman.md). Server to client: the
// figure's arrow is drawn, its damage the server's.
export interface ArrowShotMessage {
    t: MsgType.ArrowShot;
    idx: number;
    weaponId: number;
    ammoId: number;
    power: number;
    aimAngle: number;
    aimHeading: number;
}
