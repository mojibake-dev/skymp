import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { UpdateActionMessage } from "../messages/updateActionMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { isMovementControllerEvent } from "../../sync/animation";
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
// on the figure through the same entry (view/formView.ts). The player's
// melee and voice keep the event relay (sync/animation.ts): the server's
// AnimationSystem reads attackStart and kin, and shoutStart works as it is.
//
// thuum docs/verbs/npc-stream.md (ADR-050 decision 1): an actor this game
// hosts (the hosted set HostService.markHosted keeps in the fork's Skyrim
// Platform, setHosted) is reported by the same hook, and every action it
// performs goes as UpdateAction under the actor's own id: the puppets
// perform it through the same entry, and the relay's source drops the graph
// event the action sent (sync/animation.ts, isPerformingAction), so each
// plays once. The server's gate is ActorUpdatableBy's (the host of the
// actor's cell); a report about an actor this game stopped hosting a frame
// ago is dropped there.
const HAND_CAST_EVENT = /^(mrh_|mlh_|dualmagic)/i;

export const isHandCastEvent = (animEventName: string): boolean => HAND_CAST_EVENT.test(animEventName);

export class ActionSyncService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("actionPerformed", (e) => this.onActionPerformed(e));
    }

    private onActionPerformed(e: { actorId: number; action: number; idle: number; eventName: string; type: number; variables: { booleans: ArrayBuffer; floats: ArrayBuffer; integers: ArrayBuffer } }) {
        const isPlayer = e.actorId === 0x14;
        if (isPlayer && !isHandCastEvent(e.eventName)) {
            return;
        }
        // a hosted actor's movement controller events (moveStart, the turns)
        // travel by no channel: a puppet's legs are the engine's locomotion
        // channels fed from the movement relay (docs/verbs/npc-puppet.md), and
        // a replayed moveStop would idle its graph against the fed speed
        if (!isPlayer && isMovementControllerEvent(e.eventName)) {
            return;
        }
        // the event's context allows no game call (x-figure-action
        // 20261015-053450: "'Form.getFormID' can't be called in this
        // context"), so the message is built here and sent on the next update:
        // the player's under its own idx (sendMessageWithRefrId maps an unset
        // _refrId to it), a hosted actor's under its remote id, mapped on the
        // update (a server-made copy's id goes through the view's map)
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
            if (!isPlayer) {
                const remoteId = localIdToRemoteId(e.actorId);
                if (!remoteId) {
                    logTrace(this, `action ${e.action.toString(16)} ${e.eventName} on ${e.actorId.toString(16)} dropped: no remote id`);
                    return;
                }
                message._refrId = remoteId;
            }
            logTrace(this, `action ${e.action.toString(16)} idle ${e.idle.toString(16)} ${e.eventName} type ${e.type} on ${e.actorId.toString(16)}`);
            this.controller.emitter.emit("sendMessageWithRefrId", {
                message,
                reliability: "reliable"
            });
        });
    }
}
