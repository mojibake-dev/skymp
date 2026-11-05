import { Actor } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { TimersService } from "./timersService";
import { logLab } from "../../logging";

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
        this.setLocalDamageMult(0);
        let done = false;
        const finish = (how: string) => {
            if (done) {
                return;
            }
            done = true;
            this.controller.once("update", () => {
                this.setLocalDamageMult(this.defaultLocalDamageMult());
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
    // damage to the player alone: a hit with no attacker (a trap, a fall,
    // lava) or the player's own. They were 0, the old way of keeping an
    // actor's hit off the player's own engine (a relayed Fire Breath took
    // test 2's own game to 0.34 while the server counted half, x-shout-relay
    // 20261013-082608; docs/verbs/shouts.md), which zeroed the world's damage
    // too: a spike wall's hit cost the player nothing (x-trap-hit-premise
    // 20261105-154820; 60 at 1.0). The client setting worldDamageMult sets
    // them, 1 by default (D-T4's recommendation, the game's own base); the
    // ragdoll removal still writes 0 for its window and this value after it
    private defaultLocalDamageMult(): number {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        const v = s ? s["worldDamageMult"] : undefined;
        return typeof v === "number" && Number.isFinite(v) && v >= 0 ? v : 1;
    }
}
