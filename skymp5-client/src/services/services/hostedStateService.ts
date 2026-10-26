import { Actor, PackageEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { HostedActorStateMessage } from "../messages/hostedActorStateMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";
import { HostService } from "./hostService";
import { localIdToRemoteId, remoteIdToLocalId } from "../../view/worldViewMisc";
import { logTrace } from "../../logging";

// thuum docs/verbs/npc-stream.md (ADR-050 decision 1, slice 2): the state of
// the actors this game simulates, read from its engine twice a second and
// sent on a change as HostedActorState under each actor's own id: the three
// percentages, the combat target (a remote id) and the running package. The
// values are the host's word bounded by the server's numbers (wire-rules
// hosted_state: a report that would undo a value the server lowered or
// raised is dropped, a health of 0 on a living record is floored at one
// point until ADR-050's deaths-by-the-engine train); the target and the
// package are recorded (R2) for npc-handoff and the lab. The first sample
// for an actor goes reliable (the keyframe after a grant), the rest
// unreliable (absolute; the newest wins). DeathReportService's poll shape.
//
// The package: no native reads it (Actor.getCurrentPackage answered one id
// for every resident, npc-stream.md's Engine surface), so the package events
// name it: packageStart and packageChange for any actor, kept per local id
// and pruned to the hosted list at each poll. The events have no native
// filter (rule 9's cost, accepted: a package change is minutes apart per
// actor); HYPOTHESIS H3 until a probe reads the 8:00 switch through them.
export class HostedStateService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
        this.controller.on("packageStart", (e) => this.onPackage(e));
        this.controller.on("packageChange", (e) => this.onPackage(e));
    }

    // the package events run in the update's context (EventHandler.cpp posts
    // them as update tasks), so the form reads are allowed here
    private onPackage(e: PackageEvent) {
        if (!e.actor) {
            return;
        }
        const localId = e.actor.getFormID();
        if (localId === 0x14) {
            return;
        }
        this.packages.set(localId, e.package ? e.package.getFormID() : 0);
    }

    private onUpdate() {
        const host = this.controller.lookupListener(HostService);
        if (!host.isStreamEnabled()) {
            return;
        }
        const now = Date.now();
        if (now - this.lastPoll < HostedStateService.pollMs) {
            return;
        }
        this.lastPoll = now;
        const hosted = host.hostedActorRemoteIds();
        const seen = new Set<number>();
        for (const remoteId of hosted) {
            if (remoteId === 0x14) {
                continue;
            }
            const localId = remoteIdToLocalId(remoteId);
            if (!localId) {
                continue;
            }
            seen.add(localId);
            const actor = this.sp.Actor.from(this.sp.Game.getFormEx(localId));
            if (!actor) {
                continue;
            }
            const sample = this.read(actor, localId);
            const last = this.lastSent.get(localId);
            if (last && HostedStateService.same(last, sample)) {
                continue;
            }
            const message: MessageWithRefrId<HostedActorStateMessage> = {
                t: MsgType.HostedActorState,
                health: sample.health,
                magicka: sample.magicka,
                stamina: sample.stamina,
                combatTarget: sample.combatTarget,
                package: sample.package,
                _refrId: remoteId,
            };
            this.controller.emitter.emit("sendMessageWithRefrId", {
                message,
                reliability: last ? "unreliable" : "reliable",
            });
            this.lastSent.set(localId, sample);
            logTrace(this, `state ${remoteId.toString(16)}: health ${sample.health.toFixed(3)} magicka ${sample.magicka.toFixed(3)} stamina ${sample.stamina.toFixed(3)} target ${sample.combatTarget.toString(16)} package ${sample.package.toString(16)}${last ? "" : " (keyframe)"}`);
        }
        // an actor that left the hosted list starts over at its next grant
        // (a keyframe again); its package is forgotten with it
        for (const id of Array.from(this.lastSent.keys())) {
            if (!seen.has(id)) {
                this.lastSent.delete(id);
            }
        }
        for (const id of Array.from(this.packages.keys())) {
            if (!seen.has(id)) {
                this.packages.delete(id);
            }
        }
    }

    private read(actor: Actor, localId: number): HostedStateSample {
        // a dead actor reads 0 (sync/actorvalues.ts's rule); the engine's
        // deferred kill can leave a living value under 0 or over 1
        // (movementGet.ts clamps the same way)
        const pct = (name: string) => {
            if (actor.isDead()) {
                return 0;
            }
            const v = actor.getActorValuePercentage(name);
            return typeof v === "number" && Number.isFinite(v) ? Math.min(1, Math.max(0, v)) : 0;
        };
        const target = actor.getCombatTarget();
        const targetLocal = target ? target.getFormID() : 0;
        return {
            health: pct("health"),
            magicka: pct("magicka"),
            stamina: pct("stamina"),
            combatTarget: targetLocal ? localIdToRemoteId(targetLocal, true) : 0,
            package: this.packages.get(localId) ?? 0,
        };
    }

    // a change worth a send: a percentage moved half a point, or the target
    // or the package changed (regeneration moves a value every frame)
    private static same(a: HostedStateSample, b: HostedStateSample): boolean {
        const near = (x: number, y: number) => Math.abs(x - y) < 0.005;
        return near(a.health, b.health) && near(a.magicka, b.magicka) && near(a.stamina, b.stamina)
            && a.combatTarget === b.combatTarget && a.package === b.package;
    }

    private readonly packages = new Map<number, number>();
    private readonly lastSent = new Map<number, HostedStateSample>();
    private lastPoll = 0;
    private static readonly pollMs = 500;
}

interface HostedStateSample {
    health: number;
    magicka: number;
    stamina: number;
    combatTarget: number;
    package: number;
}
