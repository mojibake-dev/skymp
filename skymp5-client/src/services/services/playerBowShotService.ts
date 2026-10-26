import { Actor, Ammo, Game, PlayerBowShotEvent, WeaponType } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { getEquipment } from "../../sync/equipment";
import { QueryBlockSetInventoryEvent } from "../events/queryBlockSetInventoryEvent";
import { logError, logTrace } from "../../logging";
import { PlayerBowShotMessage } from "../messages/playerBowShotMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";
import { HostService } from "./hostService";
import { localIdToRemoteId } from "../../view/worldViewMisc";

// Skyrim Platform's shot event with the aim the fork adds to it
// (skyrim-platform EventHandler.cpp, TESPlayerBowShotEvent); the published
// typings this client builds against predate the two fields
type BowShotWithAim = PlayerBowShotEvent & { aimAngle?: number, aimHeading?: number };

export class PlayerBowShotService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();

        this.controller.emitter.on("queryBlockSetInventoryEvent", (e) => this.onQueryBlockSetInventoryEvent(e));
        this.controller.on("playerBowShot", (e) => this.onPlayerBowShot(e));
        // the engine's own launch of the player's arrow (the fork's
        // projectileLaunched event: Projectile::Launch's LaunchData), kept
        // for the shot that follows it, so the observer launches the figure's
        // arrow at the same origin in the same direction (thuum
        // docs/verbs/marksman.md: the arrow's path identical on both screens)
        this.controller.on("projectileLaunched", (e) => this.onProjectileLaunched(e));
        // and its flight as it starts: the arrow's final angles, after the
        // engine's auto-aim (Launch's entry carries the angles before it;
        // the re-analyst, 2026-10-16)
        this.controller.on("projectileFlight", (e) => this.onProjectileFlight(e));
        this.controller.on("update", () => this.flushShot());

        const _this = this;

        const eventPatterns = ["attackRelease", "crossbowAttackStart"];

        eventPatterns.forEach(eventPattern => {
            this.sp.hooks.sendAnimationEvent.add({
                enter(ctx) {
                },
                leave(ctx) {
                    if (!ctx.animationSucceeded) {
                        return;
                    }

                    if (ctx.animEventName === "crossbowAttackStart") {
                        _this.score = 1;
                    } else if (ctx.animEventName === "attackRelease") {
                        if (_this.score === 1) {
                            _this.controller.once("update", () => { _this.onPlayerCrossbowShot() });
                            _this.score = 0;
                        }
                    } else {
                        _this.score = 0;
                    }
                }
            }, 0x14, 0x14, eventPattern);
        });
    }

    private onQueryBlockSetInventoryEvent(e: QueryBlockSetInventoryEvent) {
        if (Date.now() < this.inventoryUnblockMoment) {
            logTrace(this, "Blocked inventory operation");
            e.block();
        } else {
            logTrace(this, "Not blocked inventory operation");
        }
    }

    // The shot and its aim as the arrow left (thuum docs/verbs/marksman.md):
    // reliable, as the wire's contract says, since the server takes an arrow
    // for each and lets each arrow's hit through once
    private onPlayerBowShot(e: BowShotWithAim) {
        const aimHeading = e.aimHeading ?? 0;
        // the shot waits a few updates for its launch when the engine's launch
        // event has not come yet (both come at the release; their order is
        // not relied on), then goes with the launch or without
        this.pendingShot = {
            weaponId: e.weapon.getFormID(),
            ammoId: e.ammo.getFormID(),
            power: e.power,
            isSunGazing: e.isSunGazing || false,
            aimAngle: PlayerBowShotService.pitch(e.aimAngle ?? 0),
            aimHeading: Number.isFinite(aimHeading) ? aimHeading : 0,
            waited: 0,
        };
        this.flushShot();
    }

    private onProjectileLaunched(e: { shooter: number; ammo: number; weapon: number; power: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number }) {
        if (!e.ammo) {
            return; // a spell has no ammo
        }
        if (e.shooter !== 0x14) {
            this.onHostedLaunch(e);
            return;
        }
        this.lastLaunch = { at: Date.now(), ammo: e.ammo, weapon: e.weapon, originX: e.originX, originY: e.originY, originZ: e.originZ, angleX: e.angleX, angleZ: e.angleZ };
    }

    // thuum docs/verbs/npc-stream.md (train 53): an arrow a hosted actor's
    // engine launched in this game (the native launch rule lets a simulated
    // actor's arrow fly and reports it, where a figure's or a puppet's is
    // dropped) goes to the server as that actor's shot under its idx, with
    // the launch the engine made; the server relays it as ArrowShot and the
    // other games' puppets launch it the same way a figure's arrow flies
    private onHostedLaunch(e: { shooter: number; ammo: number; weapon: number; power: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number }) {
        if (!this.controller.lookupListener(HostService).isMineLocal(e.shooter)) {
            return;
        }
        const remoteId = localIdToRemoteId(e.shooter);
        const actor = this.sp.Actor.from(this.sp.Game.getFormEx(e.shooter));
        if (!remoteId || !actor) {
            logTrace(this, `hosted shot by ${e.shooter.toString(16)} dropped: no remote id or actor`);
            return;
        }
        const aim = this.aimOf(actor);
        const launchOk = [e.originX, e.originY, e.originZ, e.angleX, e.angleZ].every((v) => Number.isFinite(v));
        const weaponId = e.weapon || (actor.getEquippedWeapon(false)?.getFormID() ?? 0);
        const message: MessageWithRefrId<PlayerBowShotMessage> = {
            t: MsgType.PlayerBowShot,
            weaponId,
            ammoId: e.ammo,
            power: Number.isFinite(e.power) ? Math.max(0, Math.min(1, e.power)) : 1,
            isSunGazing: false,
            aimAngle: aim.aimAngle,
            aimHeading: aim.aimHeading,
            hasLaunch: launchOk,
            originX: launchOk ? e.originX : 0,
            originY: launchOk ? e.originY : 0,
            originZ: launchOk ? e.originZ : 0,
            launchAngleX: launchOk ? PlayerBowShotService.pitch(e.angleX) : 0,
            launchAngleZ: launchOk ? e.angleZ : 0,
            _refrId: remoteId,
        };
        this.controller.emitter.emit("sendMessageWithRefrId", { message, reliability: "reliable" });
        logTrace(this, `hosted shot by ${remoteId.toString(16)}: ${e.ammo.toString(16)} from ${weaponId.toString(16)}, power ${message.power.toFixed(2)}${launchOk ? "" : ", no launch"}`);
    }

    private onProjectileFlight(e: { shooter: number; ammo: number; x: number; y: number; z: number; angleX: number; angleZ: number }) {
        if (e.shooter !== 0x14 || !e.ammo) {
            return;
        }
        this.lastFlight = { at: Date.now(), ammo: e.ammo, x: e.x, y: e.y, z: e.z, angleX: e.angleX, angleZ: e.angleZ };
    }

    private flushShot() {
        const shot = this.pendingShot;
        if (!shot) {
            return;
        }
        const now = Date.now();
        const launchSeen = this.lastLaunch;
        const flightSeen = this.lastFlight;
        const launchFresh = !!launchSeen && now - launchSeen.at <= PlayerBowShotService.launchMatchMs && launchSeen.ammo === shot.ammoId;
        const flightFresh = !!flightSeen && now - flightSeen.at <= PlayerBowShotService.launchMatchMs && flightSeen.ammo === shot.ammoId;
        // the flight's angles are the arrow's own; the shot waits for them a
        // few updates, then goes with what it has (the launch's angles, or
        // none: the observer then launches from the figure as before)
        if (!flightFresh && shot.waited < PlayerBowShotService.launchWaitUpdates) {
            shot.waited++;
            return;
        }
        this.pendingShot = null;
        if (launchFresh) {
            this.lastLaunch = null;
        }
        if (flightFresh) {
            this.lastFlight = null;
        }
        const origin = launchFresh && launchSeen ? { x: launchSeen.originX, y: launchSeen.originY, z: launchSeen.originZ }
            : flightFresh && flightSeen ? { x: flightSeen.x, y: flightSeen.y, z: flightSeen.z } : null;
        const angles = flightFresh && flightSeen ? { x: flightSeen.angleX, z: flightSeen.angleZ }
            : launchFresh && launchSeen ? { x: launchSeen.angleX, z: launchSeen.angleZ } : null;
        const launch = origin && angles ? { originX: origin.x, originY: origin.y, originZ: origin.z, angleX: angles.x, angleZ: angles.z } : null;
        const withLaunch = !!launch && Number.isFinite(launch.originX) && Number.isFinite(launch.originY) && Number.isFinite(launch.originZ)
            && Number.isFinite(launch.angleX) && Number.isFinite(launch.angleZ);
        // the player's own shot carries the player's own idx (train 53: the
        // message names its shooter; networkingService fills it)
        const message: MessageWithRefrId<PlayerBowShotMessage> = {
            _refrId: undefined,
            t: MsgType.PlayerBowShot,
            weaponId: shot.weaponId,
            ammoId: shot.ammoId,
            power: shot.power,
            isSunGazing: shot.isSunGazing,
            aimAngle: shot.aimAngle,
            aimHeading: shot.aimHeading,
            hasLaunch: !!withLaunch,
            originX: withLaunch && launch ? launch.originX : 0,
            originY: withLaunch && launch ? launch.originY : 0,
            originZ: withLaunch && launch ? launch.originZ : 0,
            launchAngleX: withLaunch && launch ? PlayerBowShotService.pitch(launch.angleX) : 0,
            launchAngleZ: withLaunch && launch ? launch.angleZ : 0,
        };
        this.controller.emitter.emit("sendMessageWithRefrId", { message, reliability: "reliable" });
        if (!withLaunch) {
            logTrace(this, "bow shot sent without its launch:", launchFresh ? "launch seen" : "no launch", flightFresh ? "flight seen" : "no flight");
        } else if (!flightFresh) {
            logTrace(this, "bow shot sent with its launch's angles (no flight seen)");
        }
    }

    private pendingShot: { weaponId: number; ammoId: number; power: number; isSunGazing: boolean; aimAngle: number; aimHeading: number; waited: number } | null = null;
    private lastLaunch: { at: number; ammo: number; weapon: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number } | null = null;
    private lastFlight: { at: number; ammo: number; x: number; y: number; z: number; angleX: number; angleZ: number } | null = null;
    // a launch or flight this close to the shot is the shot's; a shot waits
    // this many updates for the flight (the arrow's first update follows its
    // launch by a frame or two)
    private static readonly launchMatchMs = 400;
    private static readonly launchWaitUpdates = 6;

    // A pitch within the quarter turn the server's validator allows
    private static pitch(radians: number): number {
        return Number.isFinite(radians) ? Math.max(-Math.PI / 2, Math.min(Math.PI / 2, radians)) : 0;
    }

    // A crossbow's aim, which Skyrim Platform's shot event does not carry: as
    // CommonLibSSE-NG's Actor::GetAimAngle and GetAimHeading read it, the
    // behavior graph's aim while aiming, the actor's own angles otherwise
    private aimOf(actor: Actor): { aimAngle: number, aimHeading: number } {
        const rad = Math.PI / 180;
        const aiming = actor.getAnimationVariableBool("bAimActive");
        const aimAngle = aiming ? -actor.getAnimationVariableFloat("aimPitchCurrent") : actor.getAngleX() * rad;
        const aimHeading = actor.getAngleZ() * rad - actor.getAnimationVariableFloat("aimHeadingCurrent");
        return { aimAngle: PlayerBowShotService.pitch(aimAngle), aimHeading: Number.isFinite(aimHeading) ? aimHeading : 0 };
    }

    private onPlayerCrossbowShot() {
        const actor = this.sp.Game.getPlayer();
        if (actor === null) {
          return;
        }

        const crossbow = actor.getEquippedWeapon(false);

        if (crossbow === null) {
            logError(this, `Couldn't get equipped weapon`);
            return;
        }

        const weaponType = crossbow.getWeaponType();
        if (weaponType !== WeaponType.Crossbow) {
            logError(this, `Expected weapon to be crossbow, but got WeaponType`, weaponType);
            return;
        }

        const equippedAmmoEntries = getEquipment(actor, 0).inv.entries.filter(entry => entry.worn && Ammo.from(Game.getFormEx(entry.baseId)));

        if (equippedAmmoEntries.length === 0) {
            logError(this, `Ammo not found`);
            return;
        }

        if (equippedAmmoEntries.length > 1) {
            const equippedAmmoIds = equippedAmmoEntries.map(entry => entry.baseId);
            logError(this, `Found more than 1 ammos:`, equippedAmmoIds);
            return;
        }

        // the player's own shot carries the player's own idx (train 53: the
        // message names its shooter; networkingService fills it)
        const message: MessageWithRefrId<PlayerBowShotMessage> = {
            _refrId: undefined,
            t: MsgType.PlayerBowShot,
            weaponId: crossbow.getFormID(),
            ammoId: equippedAmmoEntries[0].baseId,
            isSunGazing: false,
            power: 1.0,
            ...this.aimOf(actor),
            // a crossbow's shot goes without its launch: the figure's
            // bolt leaves along the aim (the wire requires the fields)
            hasLaunch: false,
            originX: 0,
            originY: 0,
            originZ: 0,
            launchAngleX: 0,
            launchAngleZ: 0,
        };
        this.controller.emitter.emit("sendMessageWithRefrId", { message, reliability: "reliable" });

        // Fixes race condition when the server removes an item faster than local crossbow does that: -1 total
        // Then crossbow removes one more item: -2 total
        // The item is being added back: -1 total (which is correct but looks ugly in process)
        this.inventoryUnblockMoment = Date.now() + 5 * 1000;

        logTrace(this, `Sent crossbow shot`);
    }

    private score = 0;
    private inventoryUnblockMoment = 0;
};
