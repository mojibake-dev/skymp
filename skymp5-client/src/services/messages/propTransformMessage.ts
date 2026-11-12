import { MsgType } from "../../messages";

// One rigid body's pose in a prop's sample (thuum ADR-071 decision 6, physics
// rung 2): a part of a body of several, a world object's hinged part (rung 4)
// or a dead actor's bone (rung 5), absolute in the world's frame; pos in game
// units, quat a unit quaternion as Havok keeps one, [x, y, z, w].
export interface PropBone {
    pos: [number, number, number];
    quat: [number, number, number, number];
}

// A physics prop's transform (thuum docs/verbs/prop-physics.md, ADR-058; v2,
// ADR-071 decision 6, physics rung 2): where the one seat that simulates it
// has it and how it moves, under the owner epoch that seat holds. To the
// server, from the simulator, unreliable, at most once per prop per 100 ms
// while the body moves, at least every 500 ms while it is awake or held (the
// heartbeat) and once more when its island deactivates (asleep true,
// reliable); the server records it and relays the same message to the prop's
// other seats (unreliable while awake, reliable at sleep), which follow it on
// a Keyframed body (PropService). seq counts the sender's samples within
// (refr, epoch) from 1, wrapping 65535 to 1; tMs is the sender's own clock at
// the read (ms since its client started); lin is game units a second, ang
// radians a second, both in the world's frame; held is the engine's own grab
// (Game.getPlayerGrabbedRef names the body) and never comes with asleep;
// bones is empty for an item. The server's record answer to a refused sample
// is this message at seq 0 (tMs 0, no velocity), which a client never sends.
// A seat that does not simulate the prop never sends one.
export interface PropTransformMessage {
    t: MsgType.PropTransform;
    refr: number;
    epoch: number;
    seq: number;
    tMs: number;
    pos: [number, number, number];
    rot: [number, number, number];
    lin: [number, number, number];
    ang: [number, number, number];
    asleep: boolean;
    held: boolean;
    bones: PropBone[];
}
