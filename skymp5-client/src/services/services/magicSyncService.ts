// TODO: refactor this out
import { localIdToRemoteId } from "../../view/worldViewMisc";

// @ts-expect-error (TODO: Remove in 2.10.0)
import { SpellCastEvent, Actor, printConsole, Game, getAnimationVariablesFromActor, ActorAnimationVariables, SpellType, SlotType, EquippedItemType } from 'skyrimPlatform'
import { Spell } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { logTrace } from '../../logging';

import { MsgType } from "../../messages";
import { noSpellLaunch, SpellCastMsgData, SpellCastMessage } from "../messages/spellCastMessage";
import { UpdateAnimVariablesMessageMsgData } from "../messages/updateAnimVariablesMessage";

export class MagicSyncService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
        this.controller.on("spellCast", (e) => this.onSpellCast(e));
        // thuum docs/verbs/spell-cast.md: the engine's own launch of the
        // player's spell projectiles (the fork's projectileLaunched event:
        // Projectile::Launch's LaunchData), kept for the target-location cast
        // that goes with one. A target-location effect is launched at the
        // point the caster's game picked along its aim (the re-analyst on
        // sky-re, 2026-10-16; HYPOTHESIS until a lab run), and a figure in
        // another game has no aim to pick one with, so the cast carries it,
        // as a bow shot carries its arrow's launch (playerBowShotService)
        this.controller.on("projectileLaunched", (e) => this.onProjectileLaunched(e));
        this.controller.on("update", () => this.onUpdateCasts());

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
        // thuum docs/verbs/cast-action.md: a figure's cast in this game (the
        // relay's castSpellImmediate) raises this event too, and the server
        // refused each as a cast this player may not make; only the player's
        // own and its hosted actors' go out
        const casterId = event.caster.getFormID();
        if (casterId !== this.playerId && !this.isHostedByMe(casterId)) {
            return;
        }
        const isInterruptCast = false;

        const msg: SpellCastMsgData = this.getSpellCastEventData(event, isInterruptCast);

        // thuum docs/verbs/spell-cast.md: the player's own target-location
        // cast waits a few updates for its launch when the engine's launch
        // event has not come yet (both come at the release; their order is
        // not relied on), then goes with the launch or without; every other
        // cast goes at once, as before
        if (casterId === this.playerId && MagicSyncService.launchesAtTargetLocation(event.spell)) {
            this.pendingCasts.push({ msg, waited: 0 });
            this.flushCasts(false);
        } else {
            this.sendCast(msg);
        }

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
            // a cast goes before its end: one still waiting for its launch
            // goes now, with the launch or without
            this.flushCasts(true);
            // the player's own graph: its server id has no figure in its own
            // game, so remoteIdToLocalId(caster) answered 0 and the stop died
            // reading 0's variables (x-spell-state 20261010-103607, c1.log)
            const msg: SpellCastMsgData = {
                ...cast,
                // an end launches nothing
                ...noSpellLaunch,
                interruptCast: true,
                actorAnimationVariables: this.getAnimationVariablesFromActorConverted(this.playerId),
            };

            this.sendCast(msg);
        });

    }

    private sendCast(msg: SpellCastMsgData) {
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.SpellCast, data: msg },
            reliability: "reliable"
        });
    }

    private onProjectileLaunched(e: { shooter: number; ammo: number; spell: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number }) {
        if (e.shooter !== this.playerId || !e.spell || e.ammo) {
            return; // not the player's own spell (an arrow has its ammo)
        }
        const now = Date.now();
        this.launches = this.launches.filter((l) => now - l.at <= MagicSyncService.launchMatchMs);
        this.launches.push({ at: now, spell: e.spell, originX: e.originX, originY: e.originY, originZ: e.originZ, angleX: e.angleX, angleZ: e.angleZ });
        if (this.launches.length > MagicSyncService.launchesKept) {
            this.launches.shift();
        }
        this.flushCasts(false);
    }

    private onUpdateCasts() {
        for (const pending of this.pendingCasts) {
            pending.waited++;
        }
        this.flushCasts(false);
    }

    // Sends the waiting casts in the order they were made: each with the
    // first launch of its spell seen within launchMatchMs, or without one
    // once it has waited launchWaitUpdates updates, or at once when `force`
    // (its end is going). A cast that still waits holds the ones after it
    private flushCasts(force: boolean) {
        const now = Date.now();
        while (this.pendingCasts.length > 0) {
            const pending = this.pendingCasts[0];
            const i = this.launches.findIndex((l) => l.spell === pending.msg.spell && now - l.at <= MagicSyncService.launchMatchMs);
            if (i < 0 && !force && pending.waited < MagicSyncService.launchWaitUpdates) {
                return;
            }
            this.pendingCasts.shift();
            const launch = i < 0 ? null : this.launches.splice(i, 1)[0];
            const withLaunch = !!launch && Number.isFinite(launch.originX) && Number.isFinite(launch.originY) && Number.isFinite(launch.originZ)
                && Number.isFinite(launch.angleX) && Number.isFinite(launch.angleZ);
            if (withLaunch && launch) {
                this.sendCast({
                    ...pending.msg,
                    hasLaunch: true,
                    originX: launch.originX,
                    originY: launch.originY,
                    originZ: launch.originZ,
                    launchAngleX: MagicSyncService.pitch(launch.angleX),
                    launchAngleZ: launch.angleZ,
                });
            } else {
                this.sendCast(pending.msg);
                logTrace(this, "target-location cast sent without its launch:", launch ? "launch not finite" : "no launch seen", "spell", pending.msg.spell.toString(16), "after", pending.waited, "updates");
            }
        }
    }

    // A spell the engine launches at a target location: one of its effects
    // is delivered there (MagicEffect.GetDeliveryType, SKSE: 4 is the
    // engine's MagicSystem::Delivery::kTargetLocation, CommonLibSSE-NG
    // include/RE/M/MagicSystem.h:45; Papyrus reads no spell's own delivery)
    // and has a projectile, which the engine launches through
    // Projectile::Launch at the picked point (the re-analyst, 2026-10-16). A
    // target-location spell without one raises no launch to wait for and
    // goes at once, as before: Lost Grimoire's Teleportation, whose landing
    // must not overtake its cast's permit (thuum docs/verbs/cast-teleport.md)
    private static launchesAtTargetLocation(spell: Spell | null): boolean {
        if (!spell) {
            return false;
        }
        const n = spell.getNumEffects();
        for (let i = 0; i < n; ++i) {
            const effect = spell.getNthEffectMagicEffect(i);
            if (effect && effect.getDeliveryType() === MagicSyncService.kTargetLocation && effect.getProjectile()) {
                return true;
            }
        }
        return false;
    }

    // A pitch within the quarter turn the server's validator allows
    // (playerBowShotService's)
    private static pitch(radians: number): number {
        return Number.isFinite(radians) ? Math.max(-Math.PI / 2, Math.min(Math.PI / 2, radians)) : 0;
    }

    private isHostedByMe(localId: number): boolean {
        const hosted = this.sp.storage['hosted'];
        if (!Array.isArray(hosted)) {
            return false;
        }
        const remoteId = localIdToRemoteId(localId);
        return hosted.includes(remoteId) || hosted.includes(remoteId + 0x100000000);
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
            ...noSpellLaunch,
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
    private pendingCasts: { msg: SpellCastMsgData; waited: number }[] = [];
    private launches: { at: number; spell: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number }[] = [];
    // a launch this close to its cast is the cast's, and a cast waits this
    // many updates for one: the bow shot's numbers (playerBowShotService)
    private static readonly launchMatchMs = 400;
    private static readonly launchWaitUpdates = 6;
    // the launches kept: a cast of several projectile effects launches each
    private static readonly launchesKept = 8;
    private static readonly kTargetLocation = 4;
}
