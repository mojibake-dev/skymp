import { Actor, isDamageActorReplaced } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { TimersService } from "./timersService";
import { logError, logLab } from "../../logging";

export class RagdollService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.once("update", () => this.onceUpdate());
    }

    // TODO: think about tracking ragdoll state of player
    // thuum docs/LAB.md (a teleport that never settles; b-host-quit
    // 20261025-025053 and 123011, b-npc-puppet 20261023-173905; numidium's
    // root-cause read of 2026-10-25): the engine's forceRemoveRagdollFromWorld
    // sometimes never answers, a per-session player state set at load (about
    // 3 percent of one seat's sessions in the lab; whether the call is
    // refused with the refusal dropped, CallNative.cpp, or held by the
    // functor's requeue, DelayFunctor.h, is a HYPOTHESIS), and every caller
    // here waited forever: the Teleport's move (remoteServer), the respawn
    // get-up after a death and a predicted death's take-back (deathService).
    // The wait is bounded now: the callbacks run once, on the engine's answer
    // (11 to 71 ms in green runs) or at the deadline, on the update thread
    // as before; a second call for the same actor while one is pending joins
    // it instead of stacking another unanswered call.
    public safeRemoveRagdollFromWorld = (
        actor: Actor,
        afterRemoveCallback: () => void
    ) => {
        const id = actor.getFormID();
        const pending = this.pending.get(id);
        if (pending) {
            pending.push(afterRemoveCallback);
            return;
        }
        this.pending.set(id, [afterRemoveCallback]);
        // the player's own multipliers, so the player's own removal alone
        // zeroes them for its window: every teleport of an NPC runs this too
        // (remoteServer's Teleport), and must not cut the player's world
        // damage meanwhile (D-T4's review, gap 2)
        const isPlayer = id === 0x14;
        if (isPlayer) {
            this.setLocalDamageMult(0);
        }
        let done = false;
        const finish = (how: string) => {
            if (done) {
                return;
            }
            done = true;
            this.controller.once("update", () => {
                if (isPlayer) {
                    this.setLocalDamageMult(this.defaultLocalDamageMult());
                }
                const callbacks = this.pending.get(id) ?? [];
                this.pending.delete(id);
                if (how !== "answered") {
                    logLab("ragdoll", `forceRemoveRagdollFromWorld for ${id.toString(16)} ${how}; the callers run anyway`);
                }
                for (const callback of callbacks) {
                    callback();
                }
            });
        };
        actor.forceRemoveRagdollFromWorld().then(
            () => finish("answered"),
            (err) => finish(`rejected: ${String(err)}`),
        );
        this.controller.lookupListener(TimersService).setTimeout(
            () => finish(`unanswered after ${RagdollService.deadlineMs} ms`),
            RagdollService.deadlineMs,
        );
    };

    private readonly pending = new Map<number, Array<() => void>>();
    private static readonly deadlineMs = 1000;

    private onceUpdate() {
        this.setLocalDamageMult(this.defaultLocalDamageMult());
    }

    private setLocalDamageMult(damageMult: number) {
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCE", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCH", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCL", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCN", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCVE", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCVH", damageMult);
    }

    // thuum ADR-028 and D-T4 (docs/research/trap-slice-d-s3-2026-11-05.md):
    // the server computes all damage one actor deals another, and the
    // platform's DamageActor replacement cuts it on this engine whoever the
    // attacker (FridaHooks.cpp), so these six multipliers scale the WORLD's
    // damage to the player alone: a hit with no attacker (a trap's) or the
    // player's own. They were 0, the old way of keeping an
    // actor's hit off the player's own engine (a relayed Fire Breath took
    // test 2's own game to 0.34 while the server counted half, x-shout-relay
    // 20261013-082608; docs/verbs/shouts.md), which zeroed the world's damage
    // too: a spike wall's hit cost the player nothing (x-trap-hit-premise
    // 20261105-154820; 60 at 1.0). A fall read unzeroed under that 0 (b-push),
    // so whether a fall passes through these at all is HYPOTHESIS, and the
    // static read of DamageActor (re-37335 (a)) skips them for a fall at 1.0
    // or more: a worldDamageMult above 1 would scale a trap's hit and not a
    // fall's. The client setting worldDamageMult sets them, 1 by default
    // (D-T4's recommendation, the game's own base), a per-seat lever (R3, as
    // every client setting is; traps.md); the player's own ragdoll removal
    // writes 0 for its window and this value after it. Without the platform's
    // DamageActor replacement (an older platform, or a replace that failed)
    // nothing cuts another actor's damage to the player, so they stay 0 then
    private defaultLocalDamageMult(): number {
        if (!RagdollService.cutInstalled()) {
            if (!this.cutMissingSaid) {
                this.cutMissingSaid = true;
                logError(this, "DamageActor's replacement is not in: the world multiplier stays 0 (D-T4)");
            }
            return 0;
        }
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        const v = s ? s["worldDamageMult"] : undefined;
        if (v === undefined) {
            return 1;
        }
        if (typeof v === "number" && Number.isFinite(v) && v >= 0) {
            return v;
        }
        if (!this.multRefusedSaid) {
            this.multRefusedSaid = true;
            logError(this, `worldDamageMult ${JSON.stringify(v)} refused (a number of 0 or more); 1 instead`);
        }
        return 1;
    }

    private static cutInstalled(): boolean {
        try {
            return isDamageActorReplaced() === true;
        } catch (e) {
            return false;
        }
    }

    private cutMissingSaid = false;
    private multRefusedSaid = false;
}
