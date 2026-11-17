import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { UpdateActionMessage } from "../messages/updateActionMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { isMovementControllerEvent } from "../../sync/animation";
import { logTrace } from "../../logging";
import { HostService } from "./hostService";

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

// thuum playtest 53 (lab/.cache/probes/x-raise-rise's header): a raise's
// commander owns the body from the server's grant (ADR-060, ADR-063), but its
// engine takes the get-up at the hit, a frame or so before the grant reaches
// it, so the action entry's report (hosted actors alone) never saw it and the
// other seats had no get-up to play (x-reanimate-one-cast-pair
// 20261116-213049: trace-c1 GetUpBegin at 12311, trace-c2 none; c1.log
// 13:33:54:039 setHosted(35534, true) after it). A get-up an actor's graph
// takes while its actions do not stream from here is noted, and sent as the
// owner's ActionGetUp once they do, if that comes within riseCarryMs and the
// body still reads dead (its get-up still running). The action is
// ActionGetUp (lab/esm.py on 1.6.1170 Skyrim.esm: AACT 0xd1fdd, the id the
// owner's own report carries, c2-console 01:12:06.033); the idle goes as 0,
// since the engine's pick was not seen here (the watcher plays the event, by
// name, formView.ts)
const ACTION_GET_UP = 0xd1fdd;

export class ActionSyncService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("actionPerformed", (e) => this.onActionPerformed(e));
        this.hookRisesBeforeTheGrant();
        this.controller.on("update", () => this.sendRisesAtTheGrant());
    }

    // filtered in native to the one event (rule 9): every actor but the player
    private hookRisesBeforeTheGrant(): void {
        const taken = this.risesTaken;
        const controller = this.controller;
        this.sp.hooks.sendAnimationEvent.add({
            enter: () => { },
            leave: (ctx) => {
                if (!ctx.animationSucceeded) {
                    return;
                }
                // an actor streamed from here: the action entry reports it
                if (controller.lookupListener(HostService).isStreamedHere(ctx.selfId)) {
                    return;
                }
                taken.set(ctx.selfId, Date.now());
            },
        }, 0x15, 0xffffffff, "GetUpBegin");
    }

    private sendRisesAtTheGrant(): void {
        if (this.risesTaken.size === 0) {
            return;
        }
        const now = Date.now();
        const hostService = this.controller.lookupListener(HostService);
        this.risesTaken.forEach((at, localId) => {
            if (now - at > ActionSyncService.riseCarryMs) {
                this.risesTaken.delete(localId);
                return;
            }
            if (!hostService.isStreamedHere(localId)) {
                return;
            }
            this.risesTaken.delete(localId);
            const actor = this.sp.Actor.from(this.sp.Game.getFormEx(localId));
            const remoteId = localIdToRemoteId(localId);
            if (!actor || !actor.isDead() || !remoteId) {
                return;
            }
            const message: MessageWithRefrId<UpdateActionMessage> = {
                t: MsgType.UpdateAction,
                action: ACTION_GET_UP,
                idle: 0,
                eventName: "GetUpBegin",
                type: 2,
                actorAnimationVariables: { booleans: [], floats: [], integers: [] },
                _refrId: remoteId,
            };
            logTrace(this, `action ${ACTION_GET_UP.toString(16)} GetUpBegin on ${localId.toString(16)}: taken ${now - at} ms before its actions streamed from here, sent at the grant (its owner's raise)`);
            this.controller.emitter.emit("sendMessageWithRefrId", {
                message,
                reliability: "reliable"
            });
        });
    }

    // the get-ups taken while the actor's actions did not stream from here, by
    // local id, at the hook's time
    private readonly risesTaken = new Map<number, number>();
    private static readonly riseCarryMs = 2000;

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
