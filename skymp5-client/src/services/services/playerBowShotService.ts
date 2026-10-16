import { Actor, Ammo, Game, PlayerBowShotEvent, WeaponType } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { getEquipment } from "../../sync/equipment";
import { QueryBlockSetInventoryEvent } from "../events/queryBlockSetInventoryEvent";
import { logError, logTrace } from "../../logging";

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

    private onProjectileLaunched(e: { shooter: number; ammo: number; weapon: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number }) {
        if (e.shooter !== 0x14 || !e.ammo) {
            return; // not the player's own arrow (a spell has no ammo)
        }
        this.lastLaunch = { at: Date.now(), ammo: e.ammo, weapon: e.weapon, originX: e.originX, originY: e.originY, originZ: e.originZ, angleX: e.angleX, angleZ: e.angleZ };
    }

    private flushShot() {
        const shot = this.pendingShot;
        if (!shot) {
            return;
        }
        const launch = this.lastLaunch;
        const fresh = launch && Date.now() - launch.at <= PlayerBowShotService.launchMatchMs && launch.ammo === shot.ammoId;
        if (!fresh && shot.waited < PlayerBowShotService.launchWaitUpdates) {
            shot.waited++;
            return;
        }
        this.pendingShot = null;
        if (fresh && launch) {
            this.lastLaunch = null;
        }
        const withLaunch = fresh && launch && Number.isFinite(launch.originX) && Number.isFinite(launch.originY) && Number.isFinite(launch.originZ)
            && Number.isFinite(launch.angleX) && Number.isFinite(launch.angleZ);
        this.controller.emitter.emit("sendMessage", {
            message: {
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
            },
            reliability: "reliable"
        });
        if (!withLaunch) {
            logTrace(this, "bow shot sent without its launch", launch ? `(last launch ${Date.now() - launch.at} ms ago, ammo ${launch.ammo.toString(16)})` : "(no launch seen)");
        }
    }

    private pendingShot: { weaponId: number; ammoId: number; power: number; isSunGazing: boolean; aimAngle: number; aimHeading: number; waited: number } | null = null;
    private lastLaunch: { at: number; ammo: number; weapon: number; originX: number; originY: number; originZ: number; angleX: number; angleZ: number } | null = null;
    // a launch this close to the shot is the shot's; a shot waits this many updates for one
    private static readonly launchMatchMs = 400;
    private static readonly launchWaitUpdates = 3;

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

        this.controller.emitter.emit("sendMessage", {
            message: {
                t: MsgType.PlayerBowShot,
                weaponId: crossbow.getFormID(),
                ammoId: equippedAmmoEntries[0].baseId,
                isSunGazing: false,
                power: 1.0,
                ...this.aimOf(actor)
            },
            reliability: "reliable"
        });

        // Fixes race condition when the server removes an item faster than local crossbow does that: -1 total
        // Then crossbow removes one more item: -2 total
        // The item is being added back: -1 total (which is correct but looks ugly in process)
        this.inventoryUnblockMoment = Date.now() + 5 * 1000;

        logTrace(this, `Sent crossbow shot`);
    }

    private score = 0;
    private inventoryUnblockMoment = 0;
};
