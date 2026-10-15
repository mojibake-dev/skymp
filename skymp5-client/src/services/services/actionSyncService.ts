import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { UpdateActionMessage } from "../messages/updateActionMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";
import { logTrace } from "../../logging";

// thuum docs/verbs/cast-action.md: the figure plays the caster's actions.
// The fork's Skyrim Platform reports every BGSAction the player's actor
// performs through the engine's one action entry (actionPerformed: the
// action, the idle the engine chose, the graph event it sent, the input type,
// and the actor's behavior graph variables as the graph was asked). The
// hands' cast actions (a stream's start and release in one tick, a bolt's
// start, ready and release, the interrupt at the end, the dual idle on the
// left hand's actions) go to the server as UpdateAction, reliably, in order;
// the server relays them to the actor's neighbours, whose view performs each
// on the figure through the same entry (view/formView.ts). Melee and the
// voice keep the event relay (sync/animation.ts) in this slice: the server's
// AnimationSystem reads attackStart and kin, and shoutStart works as it is.
const HAND_CAST_EVENT = /^(mrh_|mlh_|dualmagic)/i;

export const isHandCastEvent = (animEventName: string): boolean => HAND_CAST_EVENT.test(animEventName);

export class ActionSyncService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("actionPerformed", (e) => this.onActionPerformed(e));
    }

    private onActionPerformed(e: { action: number; idle: number; eventName: string; type: number; variables: { booleans: ArrayBuffer; floats: ArrayBuffer; integers: ArrayBuffer } }) {
        if (!isHandCastEvent(e.eventName)) {
            return;
        }
        // the event's context allows no game call (x-figure-action
        // 20261015-053450: "'Form.getFormID' can't be called in this
        // context"), so the message is built here and sent on the next update;
        // the actor is the player's own (the hook filters to it), which
        // sendMessageWithRefrId maps to the player's idx when _refrId is unset
        const message: MessageWithRefrId<UpdateActionMessage> = {
            t: MsgType.UpdateAction,
            action: e.action,
            idle: e.idle,
            eventName: e.eventName,
            type: e.type,
            actorAnimationVariables: {
                booleans: Array.from(new Uint8Array(e.variables.booleans)),
                floats: Array.from(new Uint8Array(e.variables.floats)),
                integers: Array.from(new Uint8Array(e.variables.integers)),
            },
            _refrId: undefined,
        };
        this.controller.once("update", () => {
            logTrace(this, `action ${e.action.toString(16)} idle ${e.idle.toString(16)} ${e.eventName} type ${e.type}`);
            this.controller.emitter.emit("sendMessageWithRefrId", {
                message,
                reliability: "reliable"
            });
        });
    }
}
