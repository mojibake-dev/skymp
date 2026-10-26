import { Actor, PackageEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { HostedActorStateMessage } from "../messages/hostedActorStateMessage";
import { UpdateEquipmentMessage } from "../messages/updateEquipmentMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";
import { HostService } from "./hostService";
import { RemoteServer } from "./remoteServer";
import { localIdToRemoteId, remoteIdToLocalId } from "../../view/worldViewMisc";
import { getEquipedSpell, SpellType } from "../../sync/equipment";
import { Inventory } from "../../sync/inventory";
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
//
// The equipment (slice 3): the same poll reads each hosted actor's hands
// (getEquippedObject 1 and 0, two calls) and, when they change or at the
// first poll for the actor, sends UpdateEquipment under the actor's idx with
// what it wears (the worn slots and the hands, worn flags set; never its
// whole inventory, which is npc-fate's record) and the hands' spells; the
// server checks the worn items against the actor's own record and the
// puppets' hands follow (view/formView.ts applyHands). The record's hands
// are what the server's hit path judges a hosted actor's swing against
// (E_HIT_WEAPON, playtest 34: two bandits' every swing dropped), so this is
// the stream that makes a hosted NPC's hits count. A poll, not the equip
// events: whether the engine raises them for an AI's equip is unread (H4),
// and a cell's attach would raise one per resident's outfit at once.
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
        for (const remoteId of hosted) {
            if (remoteId === 0x14) {
                continue;
            }
            const localId = remoteIdToLocalId(remoteId);
            const actor = localId ? this.sp.Actor.from(this.sp.Game.getFormEx(localId)) : null;
            if (!actor || actor.isDead()) {
                continue;
            }
            const right = actor.getEquippedObject(1)?.getFormID() ?? 0;
            const left = actor.getEquippedObject(0)?.getFormID() ?? 0;
            const prev = this.hands.get(localId);
            if (prev && prev.right === right && prev.left === left) {
                continue;
            }
            this.hands.set(localId, { right, left });
            this.sendEquipment(actor, localId, remoteId, right, left);
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
        for (const id of Array.from(this.hands.keys())) {
            if (!seen.has(id)) {
                this.hands.delete(id);
                this.equipChanges.delete(id);
            }
        }
    }

    // what the actor wears, as the server's equipment lists it: the worn
    // slots (getWornForm over the 32 biped slots, armor and the shield) and
    // the hands, each entry a count of one with its worn flag (worn the
    // right hand and the slots, wornLeft the left hand), the hands' spells;
    // numChanges counts up from the record's own (the view applies on a
    // change of the number)
    private sendEquipment(actor: Actor, localId: number, remoteId: number, right: number, left: number) {
        const entries: Inventory["entries"] = [];
        const add = (baseId: number, wornLeft: boolean) => {
            if (!baseId) {
                return;
            }
            const have = entries.find((e) => e.baseId === baseId);
            if (have) {
                if (wornLeft) {
                    have.wornLeft = true;
                } else {
                    have.worn = true;
                }
                return;
            }
            entries.push(wornLeft ? { baseId, count: 1, wornLeft: true } : { baseId, count: 1, worn: true });
        };
        for (let slot = 0; slot < 32; ++slot) {
            add(actor.getWornForm(1 << slot)?.getFormID() ?? 0, false);
        }
        add(right, false);
        add(left, true);
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const recorded = world.forms.find((f) => f !== undefined && f.refrId === remoteId)?.equipment?.numChanges ?? 0;
        const numChanges = (this.equipChanges.get(localId) ?? recorded) + 1;
        this.equipChanges.set(localId, numChanges);
        const message: MessageWithRefrId<UpdateEquipmentMessage> = {
            t: MsgType.UpdateEquipment,
            data: {
                inv: { entries },
                leftSpell: getEquipedSpell(actor, SpellType.Left),
                rightSpell: getEquipedSpell(actor, SpellType.Right),
                voiceSpell: getEquipedSpell(actor, SpellType.Voice),
                instantSpell: getEquipedSpell(actor, SpellType.Instant),
                numChanges,
            },
            _refrId: remoteId,
        };
        this.controller.emitter.emit("sendMessageWithRefrId", { message, reliability: "reliable" });
        logTrace(this, `equipment ${remoteId.toString(16)}: ${entries.length} worn, right ${right.toString(16)} left ${left.toString(16)}, numChanges ${numChanges}`);
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
    private readonly hands = new Map<number, { right: number; left: number }>();
    private readonly equipChanges = new Map<number, number>();
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
