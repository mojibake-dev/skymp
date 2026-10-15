import { MsgType } from "../../messages";

// thuum docs/verbs/cast-action.md: an action the player's actor performed
// through the engine's action entry (the BGSAction, the idle the engine chose,
// the graph event it sent, the input type) with the actor's behavior graph
// variables as the graph was asked; relayed to the actor's neighbours, whose
// figures replay it (skymp-wire wire-schema skymp.rs, UpdateAction, MsgType 44)
export interface UpdateActionMessage {
    t: MsgType.UpdateAction;
    idx: number;
    action: number;
    idle: number;
    eventName: string;
    type: number;
    actorAnimationVariables: {
        booleans: number[];
        floats: number[];
        integers: number[];
    };
}
