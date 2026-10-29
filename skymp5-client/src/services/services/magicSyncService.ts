// TODO: refactor this out
import { localIdToRemoteId } from "../../view/worldViewMisc";

// @ts-expect-error (TODO: Remove in 2.10.0)
import { SpellCastEvent, Actor, printConsole, Game, getAnimationVariablesFromActor, ActorAnimationVariables, SpellType, SlotType, EquippedItemType } from 'skyrimPlatform'
import { Spell, MagicEffect } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { HostService } from './hostService';
import { logTrace } from '../../logging';
import { landsOnItsTarget } from "../../sync/spellDelivery";

import { MsgType } from "../../messages";
import { noSpellLaunch, SpellCastMsgData, SpellCastMessage } from "../messages/spellCastMessage";
import { UpdateAnimVariablesMessageMsgData } from "../messages/updateAnimVariablesMessage";

// a hosted actor's spell launch waiting for its cast event (onHostedSpellLaunch)
interface PendingHostedLaunch {
    casterLocal: number;
    spell: number;
    castingSource: number;
    originX: number;
    originY: number;
    originZ: number;
    angleX: number;
    angleZ: number;
    at: number;
    waited: number;
}

// a hosted caster's projectile cast event held for its launch (onSpellCast):
// the event's message as built in its handler, and when the event came
interface HeldHostedCast {
    casterLocal: number;
    msg: SpellCastMsgData;
    at: number;
}

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
        this.controller.on("actionPerformed", (e) => this.onHostedAction(e));

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
            // a hosted caster's cast its launch reported moments ago (the
            // creature path below): the event's report would double it
            const key = casterId !== this.playerId ? MagicSyncService.castKey(casterId, msg.spell) : null;
            const launchAt = key ? this.hostedLaunchCasts.get(key) : undefined;
            if (key && launchAt !== undefined && Date.now() - launchAt <= MagicSyncService.launchMatchMs) {
                logTrace(this, `hosted cast ${msg.spell.toString(16)} by ${casterId.toString(16)}: reported from its launch already`);
            } else if (key && (MagicSyncService.launchesAtTargetLocation(event.spell) || MagicSyncService.launchesAProjectile(event.spell))) {
                // playtest 37 (the 3i lay, 2026-10-28), Eli on the hosted flame
                // atronach's Firebolt: "yes 1 firebolt, but they dont land in
                // the same place. our marksman bug of old". The event came
                // first at every cast, so this branch sent the cast with no
                // launch and the launch a tick later was dropped as reported
                // (flushHostedLaunches); the watcher's puppet then launched
                // from its own pose. A hosted caster's projectile cast is held
                // for its launch up to launchMatchMs instead, as the player's
                // own waits (pendingCasts above), and goes as one message with
                // the launch's geometry (mergeHeldHostedCasts); with no launch
                // by then it goes as before. Self and touch casts keep the
                // immediate send below
                this.heldHostedCasts.push({ casterLocal: casterId, msg, at: Date.now() });
                this.mergeHeldHostedCasts();
            } else {
                this.sendCast(msg);
            }
            if (key) {
                this.hostedEventCasts.set(key, Date.now());
            }
            // thuum docs/verbs/npc-stream.md (train 53): a hosted caster's
            // end comes through its action stream (the hand's equipped event,
            // actionPerformed), not the player's own graph hook below
            if (casterId !== this.playerId) {
                let byHand = this.lastHostedCastByHand.get(casterId);
                if (!byHand) {
                    byHand = new Map<number, SpellCastMsgData>();
                    this.lastHostedCastByHand.set(casterId, byHand);
                }
                byHand.set(msg.castingSource, msg);
            }
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

    private onProjectileLaunched(e: { shooter: number; projectile: number; ammo: number; spell: number; castingSource: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number }) {
        if (e.shooter !== this.playerId && e.spell && !e.ammo) {
            // a hosted actor's spell launch (the native hook admits the hosted
            // set): its natives wait for the update tick, the launch event's
            // context refusing them (memo D44, playerBowShotService's hosted
            // branch). The puppet count below reads the native flag, so it
            // waits with them
            this.controller.once("update", () => {
                this.countPuppetLaunch(e.shooter, e.spell);
                this.onHostedSpellLaunch(e);
            });
            return;
        }
        if (e.shooter !== this.playerId || !e.spell || e.ammo) {
            return; // not the player's own spell (an arrow has its ammo)
        }
        this.noteLaunch(e.spell, e.originX, e.originY, e.originZ, e.angleX, e.angleZ, e.projectile);
    }

    // thuum docs/verbs/npc-stream.md (train 53; playtest 37's flame atronach,
    // 2026-10-29: "the fire bolt from the flame atronoch was visible for
    // prisoner only"): the host's engine launched a hosted creature's bolt and
    // no spellCast event reported the cast (which engine event a creature's
    // graph raises is the Dynamic plan's read), so nothing reached the
    // server, the watcher's puppet cast nothing and the bolt's hits were
    // refused. The launch itself names the caster, the spell, the hand and
    // the bolt's origin and angles (Projectile::Launch's LaunchData, the
    // fork's projectileLaunched), so a hosted caster's cast is reported from
    // its launch when no cast event claims it within the match window: the
    // bow relay's shape (playerBowShotService.onHostedLaunch). An event that
    // comes first makes the launch's report redundant; one that comes after
    // is dropped the same way (onSpellCast). A creature's end never comes
    // (creatures ride the event relay, not the action stream), and a bolt
    // needs none
    private onHostedSpellLaunch(e: { shooter: number; spell: number; castingSource: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number }) {
        if (!this.isHostedByMe(e.shooter)) {
            return;
        }
        this.pendingHostedLaunches.push({
            casterLocal: e.shooter, spell: e.spell, castingSource: e.castingSource,
            originX: e.originX, originY: e.originY, originZ: e.originZ, angleX: e.angleX, angleZ: e.angleZ,
            at: Date.now(), waited: 0,
        });
        this.flushHostedLaunches();
    }

    // the hosted launches waiting for a cast event: one held for this launch
    // takes its geometry and goes (mergeHeldHostedCasts); one that came
    // within the match window makes the launch's report redundant; a launch
    // that has waited launchWaitUpdates updates goes as the cast. A held cast
    // whose window closed with no launch goes as the event reported it
    private flushHostedLaunches() {
        const now = Date.now();
        const stale: string[] = [];
        this.hostedEventCasts.forEach((at, key) => {
            if (now - at > MagicSyncService.hostedCastsKeptMs) {
                stale.push(key);
            }
        });
        stale.forEach((key) => this.hostedEventCasts.delete(key));
        stale.length = 0;
        this.hostedLaunchCasts.forEach((at, key) => {
            if (now - at > MagicSyncService.hostedCastsKeptMs) {
                stale.push(key);
            }
        });
        stale.forEach((key) => this.hostedLaunchCasts.delete(key));
        this.mergeHeldHostedCasts();
        const keep: PendingHostedLaunch[] = [];
        for (const pending of this.pendingHostedLaunches) {
            const key = MagicSyncService.castKey(pending.casterLocal, pending.spell);
            const eventAt = this.hostedEventCasts.get(key);
            if (eventAt !== undefined && Math.abs(eventAt - pending.at) <= MagicSyncService.launchMatchMs) {
                logTrace(this, `hosted launch of ${pending.spell.toString(16)} by ${pending.casterLocal.toString(16)}: its cast event reported it`);
                continue;
            }
            if (pending.waited < MagicSyncService.launchWaitUpdates) {
                pending.waited++;
                keep.push(pending);
                continue;
            }
            this.sendHostedLaunchCast(pending);
        }
        this.pendingHostedLaunches = keep;
        const stillHeld: HeldHostedCast[] = [];
        for (const held of this.heldHostedCasts) {
            if (now - held.at > MagicSyncService.launchMatchMs) {
                this.sendCast(held.msg);
                logTrace(this, `hosted cast ${held.msg.spell.toString(16)} by ${held.casterLocal.toString(16)}: no launch within the window, sent from its event`);
            } else {
                stillHeld.push(held);
            }
        }
        this.heldHostedCasts = stillHeld;
    }

    // the held cast events paired with the pending launches of the same
    // caster and spell within launchMatchMs: each pair goes as one message,
    // the event's cast with the launch's geometry (the shape the player's own
    // flushCasts sends), so the watcher launches the bolt where the host's
    // engine did. Called from both sides, so either order pairs: the event's
    // handler holds and pairs at once with a launch already waiting (its data
    // read on its own update tick, memo D44), and a launch pairs on that tick
    // with an event held before it. The merged message needs no native: the
    // event's message was built in its handler and the launch is numbers
    private mergeHeldHostedCasts() {
        if (this.heldHostedCasts.length === 0 || this.pendingHostedLaunches.length === 0) {
            return;
        }
        const launchesLeft: PendingHostedLaunch[] = [];
        for (const pending of this.pendingHostedLaunches) {
            const i = this.heldHostedCasts.findIndex((held) => held.casterLocal === pending.casterLocal
                && held.msg.spell === pending.spell && Math.abs(held.at - pending.at) <= MagicSyncService.launchMatchMs);
            if (i < 0) {
                launchesLeft.push(pending);
                continue;
            }
            this.sendHeldHostedCast(this.heldHostedCasts.splice(i, 1)[0], pending);
        }
        this.pendingHostedLaunches = launchesLeft;
    }

    private sendHeldHostedCast(held: HeldHostedCast, launch: PendingHostedLaunch) {
        const launchOk = [launch.originX, launch.originY, launch.originZ, launch.angleX, launch.angleZ].every((v) => Number.isFinite(v));
        if (!launchOk) {
            // a launch the wire would refuse (E_VAL_NONFINITE) attaches
            // nothing; the event's cast goes as before
            this.sendCast(held.msg);
            logTrace(this, `hosted cast ${held.msg.spell.toString(16)} by ${held.casterLocal.toString(16)}: its launch not finite, sent from its event`);
            return;
        }
        this.sendCast({
            ...held.msg,
            hasLaunch: true,
            originX: launch.originX,
            originY: launch.originY,
            originZ: launch.originZ,
            launchAngleX: MagicSyncService.pitch(launch.angleX),
            launchAngleZ: launch.angleZ,
        });
        logTrace(this, `hosted cast ${held.msg.spell.toString(16)} by ${held.casterLocal.toString(16)}: the launch's geometry attached`);
    }

    private sendHostedLaunchCast(pending: PendingHostedLaunch) {
        const remoteId = localIdToRemoteId(pending.casterLocal, true);
        if (!remoteId) {
            logTrace(this, `hosted launch of ${pending.spell.toString(16)} by ${pending.casterLocal.toString(16)} dropped: no remote id`);
            return;
        }
        // the launch's casting source (MagicSystem::CastingSource, 0 left to
        // 3 instant, the validator's range); the right hand when it names none
        const hand = pending.castingSource >= 0 && pending.castingSource <= 3 ? pending.castingSource : 1;
        const launchOk = [pending.originX, pending.originY, pending.originZ, pending.angleX, pending.angleZ].every((v) => Number.isFinite(v));
        const msg: SpellCastMsgData = {
            caster: remoteId,
            target: 0,
            spell: pending.spell,
            interruptCast: false,
            isDualCasting: false,
            castingSource: hand,
            aimAngle: MagicSyncService.pitch(pending.angleX),
            aimHeading: Number.isFinite(pending.angleZ) ? pending.angleZ : 0,
            hasLaunch: launchOk,
            originX: launchOk ? pending.originX : 0,
            originY: launchOk ? pending.originY : 0,
            originZ: launchOk ? pending.originZ : 0,
            launchAngleX: launchOk ? MagicSyncService.pitch(pending.angleX) : 0,
            launchAngleZ: launchOk ? pending.angleZ : 0,
            // a creature's graph gives none (the master behaviour's alone are
            // read); the other games' puppets cast with their own set
            actorAnimationVariables: this.getAnimationVariablesFromActorConverted(pending.casterLocal),
        };
        this.sendCast(msg);
        this.hostedLaunchCasts.set(MagicSyncService.castKey(pending.casterLocal, pending.spell), Date.now());
        logTrace(this, `hosted cast from its launch: ${remoteId.toString(16)} casts ${pending.spell.toString(16)} hand ${hand}${launchOk ? "" : ", no launch"}`);
    }

    private static castKey(casterLocal: number, spell: number): string {
        return `${casterLocal}:${spell}`;
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
        if (this.pendingHostedLaunches.length > 0 || this.heldHostedCasts.length > 0 || this.hostedEventCasts.size > 0 || this.hostedLaunchCasts.size > 0) {
            this.flushHostedLaunches();
        }
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

    // thuum docs/verbs/cell-host.md: an actor this game hosts (its cell
    // granted here, the server marking it nobody else's)
    private isHostedByMe(localId: number): boolean {
        return this.controller.lookupListener(HostService).isMineLocal(localId);
    }

    // thuum docs/verbs/npc-stream.md (train 53, H-53b): a hosted caster's cast
    // ends when its graph plays the hand's equipped event, which the host's
    // action stream carries (ActionSyncService's actionPerformed, filtered to
    // the hosted set in native); the end goes under the caster's own id, as
    // the player's does from its own graph hook (onSendAnimationEventLeave)
    private onHostedAction(e: { actorId: number; eventName: string }) {
        if (e.actorId === this.playerId) {
            return;
        }
        const hand = this.handOfEquippedAnim(e.eventName);
        if (hand === null) {
            return;
        }
        const byHand = this.lastHostedCastByHand.get(e.actorId);
        const cast = byHand?.get(hand);
        if (!byHand || !cast) {
            return;
        }
        byHand.delete(hand);
        const msg: SpellCastMsgData = {
            ...cast,
            ...noSpellLaunch,
            interruptCast: true,
            actorAnimationVariables: this.getAnimationVariablesFromActorConverted(e.actorId),
        };
        this.sendCast(msg);
    }

    private lastHostedCastByHand = new Map<number, Map<number, SpellCastMsgData>>();
    // the hosted casts' reports by caster and spell: the cast event's and the
    // launch's, each the other's dedupe within launchMatchMs
    private hostedEventCasts = new Map<string, number>();
    private hostedLaunchCasts = new Map<string, number>();
    private pendingHostedLaunches: PendingHostedLaunch[] = [];
    // the hosted casters' projectile cast events held for their launches, up
    // to launchMatchMs each (onSpellCast, mergeHeldHostedCasts)
    private heldHostedCasts: HeldHostedCast[] = [];
    private static readonly hostedCastsKeptMs = 5000;

    // playtest 37, finding 53 (2026-10-28): the spell launches of the puppets
    // on this seat, counted per shooter in the console log. The read found
    // the host's extra bolts (one per relayed copy) and left the watcher's
    // own second bolt undetermined; with this line a run counts a puppet's
    // launches against its relayed casts (remoteServer's "cast ... on
    // puppet" line). A count only: the launch itself is left as it was
    private puppetLaunchCounts = new Map<number, number>();

    private countPuppetLaunch(shooter: number, spell: number): void {
        if (!this.sp.isPuppet(shooter)) {
            return;
        }
        const n = (this.puppetLaunchCounts.get(shooter) ?? 0) + 1;
        this.puppetLaunchCounts.set(shooter, n);
        logTrace(this, `spell launch ${spell.toString(16)} by puppet ${shooter.toString(16)}, launch ${n} of this puppet's on this seat`);
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
