import { Actor, PackageEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { HostedActorStateMessage } from "../messages/hostedActorStateMessage";
import { UpdateEquipmentMessage } from "../messages/updateEquipmentMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";
import { HostService } from "./hostService";
import { LeadClaimService } from "./leadClaimService";
import { RemoteServer } from "./remoteServer";
import { localIdToRemoteId, remoteIdToLocalId } from "../../view/worldViewMisc";
import { getEquipedSpell, SpellType } from "../../sync/equipment";
import { Inventory, readWorn } from "../../sync/inventory";
import { logLab, logTrace } from "../../logging";

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
// thuum docs/verbs/npc-fate.md (playtest 37's finding 54): the engine's
// isDead is tracked per hosted actor, and the sample at its flip from dead
// to alive (a necromancer's reanimate taken by the host's engine, Skyrim
// Platform's HostedResurrect hook) goes reliable and is never swallowed as
// unchanged: it is the server's one notice that the body stands
// (ActionListener::OnHostedActorState clears the record's death on it).
//
// The package: no native reads it (Actor.getCurrentPackage answered one id
// for every resident, npc-stream.md's Engine surface), so the package events
// name it: packageStart and packageChange for any actor, kept per local id
// and pruned to the hosted list at each poll. The events have no native
// filter (rule 9's cost, accepted: a package change is minutes apart per
// actor); HYPOTHESIS H3 until a probe reads the 8:00 switch through them.
//
// The equipment (slice 3; fork m4-npc-worn, ADR-063: the engine that
// simulates an actor owns what it wears): the same poll reads what each
// hosted actor wears, its hands (getEquippedObject 1 and 0, two calls) and
// its engine's worn flags (sync/inventory.ts readWorn, one call: the biped
// slots' armor, a skin a script equipped, the quiver), and, when any of it
// changes, sends UpdateEquipment under the actor's idx with what it wears
// (worn flags set; never its whole inventory, which is npc-fate's record)
// and the hands' spells; the server records it (R2) and relays it, and
// every other seat's copy wears it (view/formView.ts applyNpcWorn). The
// record's hands are what the server's hit path judges a hosted actor's
// swing against (E_HIT_WEAPON, playtest 34: two bandits' every swing
// dropped), so this is also the stream that makes a hosted NPC's hits
// count. A poll, not the equip events: whether the engine raises them for
// an AI's equip is unread (H4), and a cell's attach would raise one per
// resident's outfit at once. The first report for an engine reference waits
// for two equal reads half a second apart: a copy read before its engine
// dressed it (playtest 37's finding 46, the placed ff000002 of 2026-11-10
// first read with one of its four pieces) would show every other seat a
// strip that its next report undoes.
//
// thuum docs/verbs/npc-puppet.md (Ownership; ADR-063, ADR-073 decision 1):
// this engine runs the hosted actor's effects and draws their looks, so the
// same poll's game-thread read (the platform's pollHostedGaze, read back by
// hostedEffects) names the running effects whose look it keeps: active,
// neither inactive (conditions false) nor dispelled, a hit shader or a hit
// effect art, a constant effect or FX Persist (PuppetApi.cpp ReadEffectLooks),
// less the effects whose only look flashes: neither FX Persist nor a hit
// effect art, as a disease's hit shader (readEffects, tracker #164).
// The set rides the state as `effects` on the first report with the read and
// whenever it changes (that report goes reliable), and is absent otherwise;
// the server records it (R2) and relays it, and each watching seat plays the
// looks of exactly that set on its puppet (PuppetService).
export class HostedStateService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
        this.controller.on("packageStart", (e) => this.onPackage(e, true));
        this.controller.on("packageChange", (e) => this.onPackage(e, false));
    }

    // the package events run in the update's context (EventHandler.cpp posts
    // them as update tasks), so the form reads are allowed here; started: a
    // packageStart (LeadClaimService reads the package she runs from it alone)
    private onPackage(e: PackageEvent, started: boolean) {
        if (!e.actor) {
            return;
        }
        const localId = e.actor.getFormID();
        if (localId === 0x14) {
            return;
        }
        this.packages.set(localId, e.package ? e.package.getFormID() : 0);
        // thuum ADR-073 step 5a (D-53-4): a quest's own package opens that
        // quest's lead on its alias actor, LeadClaimService's to ask; another
        // ends it (D-53-5 B)
        this.controller.lookupListener(LeadClaimService).onPackage(localId, e.package ? e.package : null, started);
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
            const dead = actor.isDead();
            const stoodUp = this.dead.get(localId) === true && !dead;
            // the alive-to-dead flip goes reliable too: it is the host's
            // dead-sample stamp on the server for a death by the server's word
            // (TickDeaths, the console's kill), which no report carries; one
            // lost packet must not refuse a later script's Resurrect (round
            // nine; finding 54's shape for the stand-up)
            const fellDead = this.dead.get(localId) === false && dead;
            this.dead.set(localId, dead);
            const sample = this.read(actor, localId);
            const last = this.lastSent.get(localId);
            // the running effects with a look: sent when the engine's read
            // differs from the set this seat last sent for the actor (its
            // first read after a grant included), and then reliable
            const effects = this.readEffects(localId, remoteId);
            const sentEffects = this.lastEffects.get(localId);
            const effectsChanged = effects !== undefined && !HostedStateService.sameIds(effects, sentEffects);
            if (last && !stoodUp && !fellDead && !effectsChanged && HostedStateService.same(last, sample)) {
                continue;
            }
            const message: MessageWithRefrId<HostedActorStateMessage> = {
                t: MsgType.HostedActorState,
                health: sample.health,
                magicka: sample.magicka,
                stamina: sample.stamina,
                combatTarget: sample.combatTarget,
                package: sample.package,
                gazeTarget: sample.gazeTarget,
                gazeSlot: sample.gazeSlot,
                emotionType: sample.emotionType,
                emotionValue: sample.emotionValue,
                _refrId: remoteId,
            };
            if (effectsChanged && effects !== undefined) {
                message.effects = effects;
            }
            this.controller.emitter.emit("sendMessageWithRefrId", {
                message,
                reliability: last && !stoodUp && !fellDead && !effectsChanged ? "unreliable" : "reliable",
            });
            this.lastSent.set(localId, sample);
            if (effectsChanged && effects !== undefined) {
                this.lastEffects.set(localId, effects);
                logLab("verb:npc-puppet", `effects ${remoteId.toString(16)}: ${effects.length} running with a look on this engine (${effects.length ? effects.map((id) => id.toString(16)).join(", ") : "none"}), sent reliable${sentEffects === undefined ? " (its first read here)" : ""}`);
            }
            if (fellDead) {
                logLab("verb:npc-fate", `${remoteId.toString(16)} fell dead on this engine: health ${sample.health.toFixed(3)}, the sample sent reliable (the host's dead-sample stamp)`);
            }
            if (stoodUp) {
                logLab("verb:npc-fate", `${remoteId.toString(16)} stood up on this engine: health ${sample.health.toFixed(3)}, the sample sent reliable for the server's record`);
            }
            logTrace(this, `state ${remoteId.toString(16)}: health ${sample.health.toFixed(3)} magicka ${sample.magicka.toFixed(3)} stamina ${sample.stamina.toFixed(3)} target ${sample.combatTarget.toString(16)} package ${sample.package.toString(16)} gaze ${sample.gazeTarget.toString(16)} slot ${sample.gazeSlot} emotion ${sample.emotionType} ${sample.emotionValue}${last ? "" : " (keyframe)"}`);
        }
        // thuum tracker #119: this seat's own player, whose effects with a
        // look its engine runs and draws, as a host's engine does its actors'
        this.reportOwnEffects();
        seen.add(0x14);
        // slice 5: the gaze and the face read on the game thread for the next
        // poll (one task; rule 9), into this seat's own table (own = true), so
        // a lab read (npc-state, its own table) never empties it (#56)
        this.sp.pollHostedGaze(Array.from(seen), true);
        for (const remoteId of hosted) {
            if (remoteId === 0x14) {
                continue;
            }
            const localId = remoteIdToLocalId(remoteId);
            const actor = localId ? this.sp.Actor.from(this.sp.Game.getFormEx(localId)) : null;
            // thuum docs/verbs/npc-fate.md (playtest 37, finding 46): the
            // engine dresses a copy at its model's load (the biped init,
            // re-npc-roll.md 8.1); polled before it the copy wears nothing
            // and its hands are empty, and that report emptied the record's
            // worn set, the fate's source, so the next fate redressed the
            // copy naked. The server takes off only what this game's own
            // earlier report wore (ActionListener::OnUpdateEquipment), and
            // the poll waits for the model and for two equal reads. HYPOTHESIS
            // until a trace: the dressing precedes the 3D-loaded flag. A copy
            // out of its 3D, or dead, is read afresh when it is back (its
            // engine may dress it again at the load)
            if (!actor || actor.isDead() || !actor.is3DLoaded()) {
                if (localId) {
                    this.worn.delete(localId);
                }
                continue;
            }
            const right = actor.getEquippedObject(1)?.getFormID() ?? 0;
            const left = actor.getEquippedObject(0)?.getFormID() ?? 0;
            const worn = readWorn(localId);
            const key = `${right}:${left}:${worn.join(",")}`;
            const prev = this.worn.get(localId);
            if (prev?.sent === key) {
                continue;
            }
            if (prev?.sent === undefined && prev?.pending !== key) {
                this.worn.set(localId, { pending: key });
                continue;
            }
            this.worn.set(localId, { sent: key });
            this.sendEquipment(actor, localId, remoteId, right, left, worn);
        }
        // an actor that left the hosted list starts over at its next grant
        // (a keyframe again); its package and its engine's isDead are
        // forgotten with it
        for (const id of Array.from(this.lastSent.keys())) {
            if (!seen.has(id)) {
                this.lastSent.delete(id);
            }
        }
        for (const id of Array.from(this.lastEffects.keys())) {
            if (!seen.has(id)) {
                this.lastEffects.delete(id);
            }
        }
        for (const id of Array.from(this.dead.keys())) {
            if (!seen.has(id)) {
                this.dead.delete(id);
            }
        }
        for (const id of Array.from(this.packages.keys())) {
            if (!seen.has(id)) {
                this.packages.delete(id);
            }
        }
        for (const id of Array.from(this.worn.keys())) {
            if (!seen.has(id)) {
                this.worn.delete(id);
                this.equipChanges.delete(id);
            }
        }
    }

    // what the actor wears, as the server's equipment lists it: what its
    // engine flags worn (readWorn: the biped slots' armor and the shield, a
    // skin, the quiver) and the hands, each entry a count of one with its
    // worn flag (worn the right hand and the rest, wornLeft the left hand),
    // the hands' spells; numChanges counts up from the record's own (the
    // view applies on a change of the number)
    private sendEquipment(actor: Actor, localId: number, remoteId: number, right: number, left: number, worn: number[]) {
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
        for (const baseId of worn) {
            if (baseId !== right && baseId !== left) {
                add(baseId, false);
            }
        }
        // a spell in a hand is the hand's spell, not a worn item (x-npc-spell
        // 221906: the mage's SparksRightHand in the worn list refused the whole
        // report, "inventory does not contain item c96a1"; train 53)
        const leftSpell = getEquipedSpell(actor, SpellType.Left);
        const rightSpell = getEquipedSpell(actor, SpellType.Right);
        if (right !== rightSpell) {
            add(right, false);
        }
        if (left !== leftSpell) {
            add(left, true);
        }
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const recorded = world.forms.find((f) => f !== undefined && f.refrId === remoteId)?.equipment?.numChanges ?? 0;
        const numChanges = (this.equipChanges.get(localId) ?? recorded) + 1;
        this.equipChanges.set(localId, numChanges);
        const message: MessageWithRefrId<UpdateEquipmentMessage> = {
            t: MsgType.UpdateEquipment,
            data: {
                inv: { entries },
                leftSpell,
                rightSpell,
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
        // slice 5: the last poll's read of the gaze and the face
        const gaze = this.sp.hostedGaze(localId, true);
        const gazeLocal = gaze && gaze[0] ? gaze[0] : 0;
        return {
            health: pct("health"),
            magicka: pct("magicka"),
            stamina: pct("stamina"),
            combatTarget: targetLocal ? localIdToRemoteId(targetLocal, true) : 0,
            package: this.packages.get(localId) ?? 0,
            gazeTarget: gazeLocal ? localIdToRemoteId(gazeLocal, true) : 0,
            gazeSlot: gaze ? Math.min(5, Math.max(0, Math.floor(gaze[1]))) : 0,
            emotionType: gaze ? Math.min(7, Math.max(0, Math.floor(gaze[2]))) : 0,
            emotionValue: gaze ? Math.min(100, Math.max(0, Math.floor(gaze[3]))) : 0,
        };
    }

    // The engine's running effects with a look on a hosted actor, from the
    // last poll's game-thread read (undefined until a read of the actor,
    // and on a platform without the export, said once): base effect ids,
    // ascending, each once, as the wire wants them. ADR-073 decision 7: what
    // the wire cannot carry falls through, each kind said once with its
    // first case and then counted at the 2nd, 4th, 8th and so on: a runtime
    // form (0xff000000 and up, this engine's own, no id elsewhere) and an
    // effect past the cap (the first 32 by id are sent)
    private readEffects(localId: number, remoteId: number): number[] | undefined {
        if (typeof this.sp.hostedEffects !== "function") {
            if (!this.effectsReaderSaid) {
                this.effectsReaderSaid = true;
                logLab("verb:npc-puppet", "effects: hostedEffects unavailable on this platform: no running effect is reported, and the watchers' puppets show no look of this seat's actors");
            }
            return undefined;
        }
        const read = this.sp.hostedEffects(localId, true);
        if (!Array.isArray(read)) {
            return undefined;
        }
        const ids: number[] = [];
        for (const id of read) {
            if (typeof id !== "number" || !Number.isInteger(id) || id <= 0) {
                continue;
            }
            if (id >= 0xff000000) {
                this.fellThrough("runtime", `${id.toString(16)} on ${remoteId.toString(16)} is a runtime form, which no other seat can resolve`);
                continue;
            }
            // thuum tracker #164 (playtest 55, Eli: "rotfern has a lingering
            // particle effect on ariadnes screen from catching brain rot"): the
            // platform's reader counts every constant effect with a look as a
            // look that stays (PuppetApi.cpp ReadEffectLooks), but a hit
            // shader stays only on an effect flagged FX Persist. A disease is
            // a constant effect that runs until it is cured: Brain Rot's
            // TrapDisDamageMagicka 0x10a251 has the HealFXS hit shader 0x12fd9,
            // no hit effect art and FX Persist clear (lab/esm.py, 2026-11-18),
            // so its once-only flash was sent as a look for as long as she
            // carried it, and every watcher drew it with no end. An effect
            // with neither FX Persist nor a hit effect art (a constant
            // effect's hit art stays, AbFXAtronachFlame's case) is a flash,
            // never sent. HYPOTHESIS until an engine read of the shaders on a
            // reference: the host's engine plays such a shader once and lets
            // it fade (the Creation Kit's reading of FX Persist; it rests on
            // Eli's look). HYPOTHESIS: SKSE's IsEffectFlagSet tests the
            // record's DATA flags word (its implementation is unread)
            const mgef = this.sp.MagicEffect.from(this.sp.Game.getFormEx(id));
            if (mgef && !mgef.isEffectFlagSet(HostedStateService.fxPersist) && !mgef.getHitEffectArt()) {
                if (!this.flashSaid.has(id)) {
                    this.flashSaid.add(id);
                    logLab("verb:npc-puppet", `effects: ${id.toString(16)} on ${remoteId.toString(16)} has a look that only flashes (no FX Persist, no hit effect art), which no watcher keeps (falls through, said once per effect; tracker #164)`);
                }
                continue;
            }
            ids.push(id);
        }
        ids.sort((a, b) => a - b);
        const unique = ids.filter((id, i) => i === 0 || ids[i - 1] !== id);
        if (unique.length > HostedStateService.maxEffects) {
            this.fellThrough("cap", `${remoteId.toString(16)} runs ${unique.length} effects with a look, past the wire's ${HostedStateService.maxEffects}; the first ${HostedStateService.maxEffects} by id are sent`);
            return unique.slice(0, HostedStateService.maxEffects);
        }
        return unique;
    }

    // thuum tracker #119 (playtest 53, Eli: "ariadne just died to sunlight
    // ... i didn't see gold fire effect on her"): the player's own effects
    // with a look are this engine's (ADR-063: a player's own state is its
    // own game's), and no other channel names them: a cast's relay carries a
    // cast's look, but an ability's effect that its conditions turn on, as
    // SunAffectsNPCVampires' _SunBurningEffect with FireCloakFXShader 0x2ACD8
    // below half Health (the mod's records, lab/esm.py), reached no other
    // seat, so the figure of a burning vampire stood bare there. The same
    // read as the hosted actors' (the poll's own table), sent on the first
    // read and on a change, reliable, as a HostedActorState under the
    // player's own idx, whose values the server leaves to ChangeValues and
    // whose set it records and relays (ActionListener::OnHostedActorState's
    // own branch); each other seat's figure of this player plays exactly it
    // (PuppetService)
    private reportOwnEffects(): void {
        const effects = this.readEffects(0x14, 0x14);
        if (effects === undefined) {
            return;
        }
        const sent = this.lastOwnEffects;
        if (HostedStateService.sameIds(effects, sent)) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return;
        }
        const pct = (av: string) => {
            const v = player.getActorValuePercentage(av);
            return Number.isFinite(v) ? Math.min(1, Math.max(0, v)) : 0;
        };
        const message: MessageWithRefrId<HostedActorStateMessage> = {
            t: MsgType.HostedActorState,
            health: pct("health"),
            magicka: pct("magicka"),
            stamina: pct("stamina"),
            combatTarget: 0,
            package: 0,
            gazeTarget: 0,
            gazeSlot: 0,
            emotionType: 0,
            emotionValue: 0,
            effects,
            _refrId: undefined,
        };
        this.controller.emitter.emit("sendMessageWithRefrId", { message, reliability: "reliable" });
        this.lastOwnEffects = effects;
        logLab("verb:npc-puppet", `effects: this seat's own player: ${effects.length} running with a look (${effects.length ? effects.map((id) => id.toString(16)).join(", ") : "none"}), sent reliable${sent === undefined ? " (its first read here)" : ""} (tracker #119)`);
    }

    private fellThrough(kind: string, first: string) {
        const n = (this.fallThrough.get(kind) ?? 0) + 1;
        this.fallThrough.set(kind, n);
        if (n === 1) {
            logLab("verb:npc-puppet", `effects: ${first} (falls through, said once; then counted)`);
        } else if ((n & (n - 1)) === 0) {
            logLab("verb:npc-puppet", `effects: ${n} ${kind} fall-through(s) so far, the last: ${first}`);
        }
    }

    private static sameIds(a: number[], b: number[] | undefined): boolean {
        return b !== undefined && a.length === b.length && a.every((id, i) => id === b[i]);
    }

    // a change worth a send: a percentage moved half a point, or the target
    // or the package changed (regeneration moves a value every frame)
    private static same(a: HostedStateSample, b: HostedStateSample): boolean {
        const near = (x: number, y: number) => Math.abs(x - y) < 0.005;
        return near(a.health, b.health) && near(a.magicka, b.magicka) && near(a.stamina, b.stamina)
            && a.combatTarget === b.combatTarget && a.package === b.package
            && a.gazeTarget === b.gazeTarget && a.gazeSlot === b.gazeSlot
            && a.emotionType === b.emotionType && Math.abs(a.emotionValue - b.emotionValue) < 10;
    }

    private readonly packages = new Map<number, number>();
    private readonly lastSent = new Map<number, HostedStateSample>();
    // the running effects last sent per hosted actor (Ownership)
    private readonly lastEffects = new Map<number, number[]>();
    // the player's own running effects with a look, last sent (#119)
    private lastOwnEffects: number[] | undefined = undefined;
    private effectsReaderSaid = false;
    private readonly fallThrough = new Map<string, number>();
    // the effects already said to only flash (#164), each said once
    private readonly flashSaid = new Set<number>();
    // MagicEffect.IsEffectFlagSet's FX Persist value: SKSE's MagicEffect.psc
    // (lab/.cache/skse-1.6.1170/Data/Scripts/Source/MagicEffect.psc:19,
    // "FXPersist 0x00001000"), the record's kFXPersist (CommonLibSSE-NG
    // include/RE/E/EffectSetting.h:57, 1 << 12)
    private static readonly fxPersist = 0x00001000;
    // the wire's bound (wire-schema cap::MAGIC_EFFECTS)
    private static readonly maxEffects = 32;
    // the engine's isDead at the last poll, per hosted actor (finding 54)
    private readonly dead = new Map<number, boolean>();
    // per hosted actor: the read last sent, or the first read waiting for
    // its equal (the first report's two reads)
    private readonly worn = new Map<number, { sent?: string; pending?: string }>();
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
    gazeTarget: number;
    gazeSlot: number;
    emotionType: number;
    emotionValue: number;
}
