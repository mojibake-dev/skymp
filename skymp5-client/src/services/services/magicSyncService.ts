// TODO: refactor this out
import { localIdToRemoteId } from "../../view/worldViewMisc";

// @ts-expect-error (TODO: Remove in 2.10.0)
import { SpellCastEvent, Actor, printConsole, Game, getAnimationVariablesFromActor, ActorAnimationVariables, SpellType, SlotType, EquippedItemType } from 'skyrimPlatform'
import { Spell, MagicEffect } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { logTrace } from '../../logging';
import { landsOnItsTarget } from "../../sync/spellDelivery";

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
        this.controller.on("projectileFlight", (e) => this.onProjectileFlight(e));
        // and the point a target-location cast with no projectile placed its
        // explosion at (the fork's areaEffectPlaced, the engine's placer
        // 34413; docs/research/apocalypse/target-location.md): carried as
        // the cast's launch too, angles zero, so the observer places the
        // same explosion there (remoteServer placeAreaEffect)
        this.controller.on("areaEffectPlaced", (e) => this.onAreaEffectPlaced(e));
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
        // not relied on), then goes with the launch or without; since
        // train-31 a fire-and-forget projectile aimed or at an actor waits
        // the same way, and for the projectile's flight after its launch, so
        // the other games' figures launch it from the caster's own origin
        // with the projectile's own angles, as arrows fly (the aim parity
        // row); every other cast goes at once, as before
        if (casterId === this.playerId && (MagicSyncService.launchesAtTargetLocation(event.spell) || MagicSyncService.launchesAProjectile(event.spell))) {
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

    private onProjectileLaunched(e: { shooter: number; projectile: number; ammo: number; spell: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number }) {
        if (e.shooter !== this.playerId || !e.spell || e.ammo) {
            return; // not the player's own spell (an arrow has its ammo)
        }
        this.noteLaunch(e.spell, e.originX, e.originY, e.originZ, e.angleX, e.angleZ, e.projectile);
    }

    // the projectile's flight as it starts (the fork's projectileFlight,
    // raised for a spell's projectiles since train-30): its angles after the
    // engine's first update, which the bow service prefers for an arrow
    // (playerBowShotService); kept for the cast that waits on it
    private onProjectileFlight(e: { shooter: number; ammo: number; spell: number; x: number; y: number; z: number; angleX: number; angleZ: number }) {
        if (e.shooter !== this.playerId || !e.spell || e.ammo) {
            return;
        }
        const now = Date.now();
        this.flights = this.flights.filter((f) => now - f.at <= MagicSyncService.launchMatchMs);
        this.flights.push({ at: now, spell: e.spell, x: e.x, y: e.y, z: e.z, angleX: e.angleX, angleZ: e.angleZ });
        if (this.flights.length > MagicSyncService.launchesKept) {
            this.flights.shift();
        }
        this.flushCasts(false);
    }

    private onAreaEffectPlaced(e: { spell: number; x: number; y: number; z: number }) {
        if (!e.spell) {
            return;
        }
        this.noteLaunch(e.spell, e.x, e.y, e.z, 0, 0);
    }

    private noteLaunch(spell: number, originX: number, originY: number, originZ: number, angleX: number, angleZ: number, projectile = 0) {
        const now = Date.now();
        this.launches = this.launches.filter((l) => now - l.at <= MagicSyncService.launchMatchMs);
        this.launches.push({ at: now, spell, originX, originY, originZ, angleX, angleZ, projectile });
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
    // first launch of its spell seen within launchMatchMs (and, for a launch
    // with a projectile, that projectile's flight: the origin is the
    // launch's, the angles the flight's, the bow service's rule), or without
    // once it has waited launchWaitUpdates updates, or at once when `force`
    // (its end is going). A cast that still waits holds the ones after it
    private flushCasts(force: boolean) {
        const now = Date.now();
        while (this.pendingCasts.length > 0) {
            const pending = this.pendingCasts[0];
            const i = this.launches.findIndex((l) => l.spell === pending.msg.spell && now - l.at <= MagicSyncService.launchMatchMs);
            const f = this.flights.findIndex((l) => l.spell === pending.msg.spell && now - l.at <= MagicSyncService.launchMatchMs);
            const flightWanted = i >= 0 && this.launches[i].projectile !== 0 && f < 0;
            if ((i < 0 || flightWanted) && !force && pending.waited < MagicSyncService.launchWaitUpdates) {
                return;
            }
            this.pendingCasts.shift();
            const launch = i < 0 ? null : this.launches.splice(i, 1)[0];
            const flight = f < 0 ? null : this.flights.splice(f, 1)[0];
            // the flight's position and angles when seen, the launch's otherwise:
            // a spell's bolt sits 2.5 to 34 units from the Launch call's origin
            // at its first update in the caster's own game (up in first person,
            // right and forward in third, b-spell-aim 20261021-042101 on
            // m2-train-31 3b579f41), where an arrow sits on its origin exactly,
            // so the figure that launched from the Launch origin flew a parallel
            // path that far off; the bolt's own first position is where it is
            const origin = flight ? { x: flight.x, y: flight.y, z: flight.z }
                : launch ? { x: launch.originX, y: launch.originY, z: launch.originZ } : null;
            const angles = flight ? { x: flight.angleX, z: flight.angleZ }
                : launch ? { x: launch.angleX, z: launch.angleZ } : null;
            const withLaunch = !!origin && !!angles && Number.isFinite(origin.x) && Number.isFinite(origin.y) && Number.isFinite(origin.z)
                && Number.isFinite(angles.x) && Number.isFinite(angles.z);
            if (withLaunch && origin && angles) {
                this.sendCast({
                    ...pending.msg,
                    hasLaunch: true,
                    originX: origin.x,
                    originY: origin.y,
                    originZ: origin.z,
                    launchAngleX: MagicSyncService.pitch(angles.x),
                    launchAngleZ: angles.z,
                });
                if (!flight && launch && launch.projectile !== 0) {
                    logTrace(this, "cast sent with its launch's angles (no flight seen)", "spell", pending.msg.spell.toString(16));
                }
            } else {
                this.sendCast(pending.msg);
                logTrace(this, "cast sent without its launch:", launch || flight ? "launch not finite" : "no launch seen", "spell", pending.msg.spell.toString(16), "after", pending.waited, "updates");
            }
        }
    }

    // A spell the engine launches at a target location: one of its effects
    // is delivered there (MagicEffect.GetDeliveryType, SKSE: 4 is the
    // engine's MagicSystem::Delivery::kTargetLocation, CommonLibSSE-NG
    // include/RE/M/MagicSystem.h:45; Papyrus reads no spell's own delivery)
    // and has a projectile, which the engine launches through
    // Projectile::Launch at the picked point (the re-analyst, 2026-10-16). A
    // target-location spell without one but with an area places its
    // explosion at the point instead (34413, raised as areaEffectPlaced), so
    // it waits too (docs/research/apocalypse/target-location.md). One with
    // neither raises nothing to wait for and goes at once, as before: Lost
    // Grimoire's Teleportation, whose landing must not overtake its cast's
    // permit (thuum docs/verbs/cast-teleport.md)
    // thuum docs/verbs/spell-cast.md (train-31, the aim parity row): a
    // fire-and-forget spell with a projectile, aimed or at a target actor
    // (Firebolt, Electrosphere, Hailstone), waits for its launch and the
    // projectile's flight as a target-location cast waits for its launch, so
    // the other games' figures launch it from the caster's own origin with
    // the projectile's angles, as arrows fly (marksman.md). CommonLibSSE-NG
    // include/RE/M/MagicSystem.h: CastingType kFireAndForget 1, Delivery
    // kAimed 2 and kTargetActor 3
    private static launchesAProjectile(spell: Spell | null): boolean {
        if (!spell) {
            return false;
        }
        const n = spell.getNumEffects();
        for (let i = 0; i < n; ++i) {
            const effect = spell.getNthEffectMagicEffect(i);
            if (!effect) {
                continue;
            }
            const delivery = effect.getDeliveryType();
            if (effect.getCastingType() === MagicSyncService.kFireAndForget
                && (delivery === MagicSyncService.kAimed || delivery === MagicSyncService.kTargetActor)
                && MagicSyncService.hasProjectile(effect, spell.getFormID())) {
                return true;
            }
        }
        return false;
    }

    private static launchesAtTargetLocation(spell: Spell | null): boolean {
        if (!spell) {
            return false;
        }
        const n = spell.getNumEffects();
        for (let i = 0; i < n; ++i) {
            const effect = spell.getNthEffectMagicEffect(i);
            if (effect && effect.getDeliveryType() === MagicSyncService.kTargetLocation
                && (MagicSyncService.hasProjectile(effect, spell.getFormID()) || spell.getNthEffectArea(i) > 0)) {
                return true;
            }
        }
        return false;
    }

    // thuum docs/research/apocalypse/target-location.md (playtest
    // twenty-three): MagicEffect.getProjectile threw in the caster's game at
    // every rune cast ("Cannot read properties of undefined (reading
    // 'prototype')": Skyrim Platform wraps the returned form by its VM
    // class name, PROJECTILE, which its class list may not carry), and the
    // throw ended the spellCast handler, so no cast reached the server and
    // the rune's hits were refused. A throw counts as a projectile (the
    // engine launches one for every rune), so the cast waits for its launch
    // as it should; the throw is logged once per spell
    private static readonly projectileThrows = new Set<number>();
    private static hasProjectile(effect: MagicEffect, spellId: number): boolean {
        try {
            return !!effect.getProjectile();
        } catch (err) {
            if (!MagicSyncService.projectileThrows.has(spellId)) {
                MagicSyncService.projectileThrows.add(spellId);
                logTrace(`magicSyncService: getProjectile threw for spell ${spellId.toString(16)}: ${String(err)}`);
            }
            return true;
        }
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
            target: this.castTarget(e),
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

    // thuum docs/verbs/spell-cast.md (x-class-target2 20261018-105815 read
    // statically, 2026-10-18): Skyrim Platform fills the spellCast event's
    // target from the caster's own MagicTarget (EventHandler.cpp, the
    // actor's GetTargetStatsObject is the actor itself), so every cast
    // named its caster and a relayed target-actor spell landed on the
    // figure that cast it (Azure Reconstruction healed the figure). For the
    // player's own cast of a spell delivered to its target (touch, target
    // actor), the target is the actor under the crosshair at the cast, by
    // its server id (a figure's through the view, an NPC's as is), or none;
    // every other cast keeps the event's word, as before. Client-reported
    // (rule 5): the server records it and relays it; a reach check at the
    // cast is the follow-up, the numbers coming only through validated hits
    private castTarget(e: SpellCastEvent): number {
        if (e.caster.getFormID() === this.playerId && e.spell && landsOnItsTarget(e.spell)) {
            const picked = Game.getCurrentCrosshairRef();
            return picked ? localIdToRemoteId(picked.getFormID(), true) : 0;
        }
        // @ts-expect-error (TODO: Remove in 2.10.0)
        return e.target ? localIdToRemoteId(e.target.getFormID(), true) : 0;
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
    private launches: { at: number; spell: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number; projectile: number }[] = [];
    private flights: { at: number; spell: number; x: number; y: number; z: number; angleX: number; angleZ: number }[] = [];
    // a launch this close to its cast is the cast's, and a cast waits this
    // many updates for one: the bow shot's numbers (playerBowShotService)
    private static readonly launchMatchMs = 400;
    private static readonly launchWaitUpdates = 6;
    // the launches kept: a cast of several projectile effects launches each
    private static readonly launchesKept = 8;
    private static readonly kTargetLocation = 4;
    private static readonly kFireAndForget = 1;
    private static readonly kAimed = 2;
    private static readonly kTargetActor = 3;
}
