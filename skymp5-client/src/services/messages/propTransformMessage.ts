import { MsgType } from "../../messages";

// A physics prop's transform (thuum docs/verbs/prop-physics.md, ADR-058): where
// the one seat that simulates it has it. To the server, from the simulator,
// unreliable, at most once per prop per 100 ms while the body is awake and
// once more when it sleeps (asleep true); the server records it and relays the
// same message to the cell's other seats (unreliable while awake, reliable at
// sleep), which impose it on a Keyframed body (PropService). A seat that
// does not simulate the prop never sends one.
export interface PropTransformMessage {
    t: MsgType.PropTransform;
    refr: number;
    pos: [number, number, number];
    rot: [number, number, number];
    asleep: boolean;
}
