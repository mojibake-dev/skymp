// TODO: refactor this out
import { localIdToRemoteId } from "../../view/worldViewMisc";

// @ts-expect-error (TODO: Remove in 2.10.0)
import { SpellCastEvent, Actor, printConsole, Game, getAnimationVariablesFromActor, ActorAnimationVariables, SpellType, SlotType, EquippedItemType } from 'skyrimPlatform'
import { ClientListener, CombinedController, Sp } from './clientListener';
import { logTrace } from '../../logging';

import { MsgType } from "../../messages";
import { SpellCastMsgData, SpellCastMessage } from "../messages/spellCastMessage";
import { UpdateAnimVariablesMessageMsgData } from "../messages/updateAnimVariablesMessage";

export class MagicSyncService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
        this.controller.on("spellCast", (e) => this.onSpellCast(e));

        const self = this;


        this.sp.hooks.sendAnimationEvent.add({
            enter: (ctx) => { },
            leave: (ctx) => {
                self.onSendAnimationEventLeave(ctx);
            }
        }, this.playerId, this.playerId);
    }

    private onUpdate() {
        if (this.isAnyMagicStuffEquiped() === false) {
            return;
        }

        if (Date.now() - this.lastSendUpdateAnimationVariables <= this.sendUpdateAnimationVariablesRateMs) {
            return;
        }

        this.lastSendUpdateAnimationVariables = Date.now();

        this.controller.once('update', () => {
            const ac = Game.getPlayer();

            if (!ac) {
                return;
            }

            const animVariables = this.getAnimationVariablesFromActorConverted(ac.getFormID());

            this.controller.emitter.emit("sendMessage", {
                message: { t: MsgType.UpdateAnimVariables, data: this.getUpdateAnimVariablesEventData(ac, animVariables) },
                reliability: "reliable"
            });
        });

    }

    private onSpellCast(event: SpellCastEvent) {
        const isInterruptCast = false;

        const msg: SpellCastMsgData = this.getSpellCastEventData(event, isInterruptCast);

        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.SpellCast, data: msg },
            reliability: "reliable"
        });

        // thuum docs/verbs/spell-cast.md: the stop below hooks the player's
        // own graph only, so only the player's own casts wait for one, a hand
        // each (MagicSystem::CastingSource, CommonLibSSE-NG
        // include/RE/M/MagicSystem.h:23-29: 0 left, 1 right). One slot for
        // every caster let a figure's cast in this game (castSpellImmediate)
        // or the other hand's take the player's stop.
        if (event.caster.getFormID() === this.playerId) {
            this.lastCastByHand.set(msg.castingSource, msg);
        }
    }

    private onSendAnimationEventLeave(ctx: { animEventName: string, animationSucceeded: boolean }) {
        const hand = this.handOfEquippedAnim(ctx.animEventName);
        const cast = hand === null ? undefined : this.lastCastByHand.get(hand);
        if (hand === null || !cast) {
            return;
        }
        this.lastCastByHand.delete(hand);

        this.controller.once('update', () => {
            // the player's own graph: its server id has no figure in its own
            // game, so remoteIdToLocalId(caster) answered 0 and the stop died
            // reading 0's variables (x-spell-state 20261010-103607, c1.log)
            const msg: SpellCastMsgData = {
                ...cast,
                interruptCast: true,
                actorAnimationVariables: this.getAnimationVariablesFromActorConverted(this.playerId),
            };

            this.controller.emitter.emit("sendMessage", {
                message: { t: MsgType.SpellCast, data: msg },
                reliability: "reliable"
            });
        });

    }

    private getSpellCastEventData(e: SpellCastEvent, isInterruptCast: boolean): SpellCastMsgData {
        const spellCastData: SpellCastMsgData = {
            caster: localIdToRemoteId(e.caster.getFormID(), true),
            // @ts-expect-error (TODO: Remove in 2.10.0)
            target: e.target ? localIdToRemoteId(e.target.getFormID(), true) : 0,
            spell: e.spell ? e.spell.getFormID() : 0,
            interruptCast: isInterruptCast,
            // @ts-expect-error (TODO: Remove in 2.10.0)
            isDualCasting: e.isDualCasting,
            // @ts-expect-error (TODO: Remove in 2.10.0)
            castingSource: e.castingSource,
            // @ts-expect-error (TODO: Remove in 2.10.0)
            aimAngle: e.aimAngle,
            // @ts-expect-error (TODO: Remove in 2.10.0)
            aimHeading: e.aimHeading,
            actorAnimationVariables: this.getAnimationVariablesFromActorConverted(e.caster.getFormID()),
        }
        return spellCastData;
    }

    private getAnimationVariablesFromActorConverted(actorId: number) {
        const animVars = getAnimationVariablesFromActor(actorId);
        const booleans: ArrayBuffer = animVars.booleans;
        const floats: ArrayBuffer = animVars.floats;
        const integers: ArrayBuffer = animVars.integers;
        return {
            booleans: Array.from(new Uint8Array(booleans)),
            floats: Array.from(new Uint8Array(floats)),
            integers: Array.from(new Uint8Array(integers)),
        }
    }

    private getUpdateAnimVariablesEventData(ac: Actor, animVariables: ActorAnimationVariables): UpdateAnimVariablesMessageMsgData {
        const animVarsData: UpdateAnimVariablesMessageMsgData = {
            actorRemoteId: localIdToRemoteId(ac.getFormID(), true),
            actorAnimationVariables: animVariables,
        }
        return animVarsData;
    }

    // the hand a cast's end returns to its equipped state, or null
    private handOfEquippedAnim(animEventName: string): number | null {
        const eventName = animEventName.toLowerCase();
        if (eventName === "mlh_equipped_event") {
            return 0;
        }
        if (eventName === "mrh_equipped_event") {
            return 1;
        }
        return null;
    };

    private isSpellCastAnim(animEventName: string): boolean {
        const eventName = animEventName.toLowerCase();

        const isSpellCastAnimForLeftHand = eventName === "mlh_spellaimedconcentrationstart" || eventName === "mlh_spellaimedstart" || eventName === "mlh_spellready_event" ||
            eventName === "mlh_spellrelease_event" || eventName === "mlh_equipped_event";

        const isSpellCastAnimForRightHand = eventName === "mrh_spellaimedconcentrationstart" || eventName === "mrh_spellaimedstart" || eventName === "mrh_spellready_event" ||
            eventName === "mrh_spellrelease_event" || eventName === "mrh_equipped_event";

        return isSpellCastAnimForLeftHand || isSpellCastAnimForRightHand;
    };

    private isAnyMagicStuffEquiped(): boolean {
        const ac = Game.getPlayer();

        if (!ac) {
            return false;
        }

        if (ac.getEquippedSpell(SpellType.Left) || ac.getEquippedSpell(SpellType.Right)) {
            return true;
        }

        if (ac.getEquippedSpell(SpellType.Voise) || ac.getEquippedSpell(SpellType.Instant)) {
            return true;
        }

        const leftHandEquipmentType = ac.getEquippedItemType(SlotType.Left);
        const rightHandEquipmentType = ac.getEquippedItemType(SlotType.Right);

        if (leftHandEquipmentType === 9 || leftHandEquipmentType === EquippedItemType.Staff ||
            rightHandEquipmentType === 9 || rightHandEquipmentType === EquippedItemType.Staff) {
            return true;
        }

        return false;
    }

    private playerId = 0x14;
    private sendUpdateAnimationVariablesRateMs = 500;
    private lastCastByHand = new Map<number, SpellCastMsgData>();
    private lastSendUpdateAnimationVariables: number = 0;
}
