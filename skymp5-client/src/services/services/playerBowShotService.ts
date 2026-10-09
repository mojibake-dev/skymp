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
        this.controller.emitter.emit("sendMessage", {
            message: {
                t: MsgType.PlayerBowShot,
                weaponId: e.weapon.getFormID(),
                ammoId: e.ammo.getFormID(),
                power: e.power,
                isSunGazing: e.isSunGazing || false,
                aimAngle: PlayerBowShotService.pitch(e.aimAngle ?? 0),
                aimHeading: Number.isFinite(aimHeading) ? aimHeading : 0
            },
            reliability: "reliable"
        });
    }

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
