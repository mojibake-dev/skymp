import { Actor, CellAttachDetachEvent, CombatEvent, ObjectReference, Spell } from "skyrimPlatform";
import { logError, logTrace } from "../../logging";
import { Movement } from "../../sync/movement";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { RemoteServer } from "./remoteServer";

// thuum docs/verbs/npc-puppet.md (ADR-045): every NPC this game does not host
// is a puppet, the engine's own placed reference (or a server-made copy) with
// its AI off, moved and animated from the host's relay; one it hosts is live
// under the engine. The switch is the fork's Skyrim Platform export
// setPuppet: Q4's demote and promote in one native call on the game thread,
// and the flag the two detours read (Puppets::Is, the one truth about an
// actor's brain on this seat).
//
// ADR-050 decision 3 (train 51): the state is computed in ONE place,
// reconcile(), from (the server's word, loaded here, dead, kind) and applied
// idempotently from every event that could change an input (the view's
// update, a cell attach or detach, the loss of a form, a resurrect, the
// combat sink) and from one two-second poll. "Loaded here" is the cell's
// attach or the 3D (ADR-051: an actor this engine does not load is released
// to its schedule after a grace, so a 3D the late fate dropped for a moment
// does not flip a loaded actor live, playtest 34's composition). The
// deferred kill is this service's too: every NPC this game shows holds at
// zero until the server's word (train 52 moves the host's own to the engine).
//
// A dead body is no puppet on any seat (playtest 37, findings 58 and 62): its
// loot is the engine's own ContainerMenu over the server's container, so a
// body is never activation-blocked and a flagged one is released.

// one look played on a puppet: an effect shader, or the VisualEffect that
// wraps an effect's hit effect art (artId 0 for a shader)
type CloakLook = { kind: "shader" | "visual"; formId: number; artId: number; abilityId: number };

export class PuppetService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.enabled = this.sp.settings["skymp5-client"]["npcPuppet"] !== false;
        // ADR-050 decision 2: with the server's number arriving as a cause,
        // the engine of the game that simulates an NPC dies by itself; the
        // deferred kill holds puppets alone. Off (npcEngineDamage false, the
        // bisect), every NPC holds at zero for the server's word as before
        this.engineDeaths = this.sp.settings["skymp5-client"]["npcEngineDamage"] !== false;
        // playtest 37 finding 60, the visuals slice: a puppet's ability shaders
        // (the atronach's cloak) are played here; off, a puppet stands bare
        this.cloakLook = this.sp.settings["skymp5-client"]["npcCloakLook"] !== false;
        if (!this.enabled) {
            logTrace(this, "npcPuppet is off in settings: no actor's AI is switched (a bisect switch, not a play mode)");
        }
        this.controller.on("cellAttach", (e) => this.onCellAttach(e));
        this.controller.on("cellDetach", (e) => this.onCellDetach(e));
        this.controller.on("combatState", (e) => this.onCombatState(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // The one function. `mine` is the server's word as the caller read it
    // (HostService.isMine over the actor's model); undefined asks here.
    // `last` is the record's last sample, placed at a promote (the relay
    // stops at the grant).
    reconcile(localId: number, mine?: boolean, last?: Movement): void {
        if (!this.enabled || localId === 0x14) {
            return;
        }
        const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(localId));
        const ac = refr ? this.sp.Actor.from(refr) : null;
        if (!ac) {
            this.forget(localId);
            return;
        }
        if (this.isFigure(localId)) {
            return;
        }
        const host = this.controller.lookupListener(HostService);
        const word = mine ?? host.isMineLocal(localId);
        const has3D = ac.is3DLoaded();
        const loadedHere = has3D || this.attached.has(localId);
        const flagged = this.sp.isPuppet(localId);
        const now = Date.now();
        if (!has3D) {
            // a shader played on an actor ends with the 3D it was attached to
            // (Papyrus's OnLoad re-play idiom), so the look is read and played
            // again on the next 3D
            this.cloakLooks.delete(localId);
        }

        // the deferred kill (re-armed after a resurrect): a puppet never dies
        // by this game's hand, the server's word kills it; an actor this game
        // simulates dies by its own engine from the server's cause (ADR-050
        // decision 2), or holds at zero too while npcEngineDamage is off
        const holdAtZero = !word || !loadedHere || !this.engineDeaths;
        if (holdAtZero && !this.deferredArmed.has(localId)) {
            ac.startDeferredKill();
            this.deferredArmed.add(localId);
        } else if (!holdAtZero && this.deferredArmed.has(localId)) {
            ac.endDeferredKill();
            this.deferredArmed.delete(localId);
        }

        if (!loadedHere) {
            // thuum ADR-051 (npc-handoff D11; b-npc-handoff-night 092208,
            // 114904): an actor this engine no longer loads is simulated by
            // nobody here and sighted by nobody while its AI is off; after
            // the grace (a 3D dropped by the late fate comes back within it,
            // playtest 34) it is released: the flag off, the AI and the
            // activation back on, left to its own schedule. The next pass
            // makes it a puppet again when it loads on a seat that does not
            // host it, and the host's roster sights it.
            host.markHosted(localId, false);
            const since = this.unloadedSince.get(localId);
            if (since === undefined) {
                this.unloadedSince.set(localId, now);
                return;
            }
            if (flagged && now - since >= PuppetService.releaseGraceMs) {
                this.sp.releasePuppet(localId);
                this.puppets.delete(localId);
                logTrace(this, `released ${localId.toString(16)}: no 3D here, left to its schedule (npc-handoff D11)`);
            }
            return;
        }
        this.unloadedSince.delete(localId);

        if (word) {
            // thuum docs/verbs/npc-stream.md (ADR-050): the word says this
            // game simulates the actor: its actions stream from here (the
            // hosted set the action hook reads)
            host.markHosted(localId, true);
            if (!flagged) {
                return;
            }
            if (last) {
                // the record's last point and heading first: the relay stops
                // at the grant (the native promote leaves the transform alone)
                ac.setPosition(last.pos[0], last.pos[1], last.pos[2]);
                ac.setAngle(last.rot[0], last.rot[1], last.rot[2]);
            }
            // the puppet's imposed state undone, the inverse of the apply's
            // (movementApply.ts: head tracking on the relayed look-at point);
            // the activation block is the native demote's and the native
            // promote lifts it (PuppetApi.cpp; playtest 32's dead Talk prompts
            // after a handoff, playtest 33's dialogue stuck on an unblocked
            // puppet)
            ac.clearLookAt();
            ac.setHeadTracking(true);
            // and the expression override the puppet period may have left
            // (the re-analyst's read of 2026-10-28: the engine never sets it
            // itself, and while set it blocks the line's own expression on
            // the live NPC; playtest 37's exprOverride true on the talker's
            // Orgnar)
            ac.clearExpressionOverride();
            this.stopCloakLook(localId, ac);
            this.sp.setPuppet(localId, false);
            // a dead body promoted here keeps the lifted block: a corpse is
            // never activation-blocked on any seat (npc-fate.md's rule as
            // revised by playtest 37's findings 58 and 62; the body's re-block
            // that stood here defeated the engine's own loot menu, and the
            // server's activation never opened a blocked body). Promote's dead
            // branch leaves the AI off (H-54e); the not-mine branch below
            // releases a flagged body the same way.
            this.puppets.delete(localId);
            logTrace(this, `live ${localId.toString(16)}`);
            return;
        }

        // not this game's to simulate: a puppet, once it has a 3D (an actor
        // still loading is left alone until it has one; the view asks again
        // every update: the gate's hang at a landing, 2026-10-21, under
        // reading: the demote must not race the loader on a half-made actor)
        host.markHosted(localId, false);
        if (ac.isDead()) {
            // thuum docs/verbs/npc-fate.md (the deaths, as revised by playtest
            // 37's findings 58 and 62): a dead NPC body is never a puppet and
            // never activation-blocked, on any seat. The loot is the server's
            // container, opened by the engine's own activation: E on an
            // unblocked body opens the ContainerMenu, the activate hook reports
            // it, the server answers the record's inventory and OpenContainer,
            // and the view lays the server's list under the open menu (memo
            // D52 B). The design note assumed the server-triggered
            // activate(player, true) opens a blocked body; it never opened a
            // demoted one. c2's one good open (finding 58) came while a brief
            // host grant had lifted the block, and after the host flipped back
            // the body was demoted again and four E presses opened nothing. A
            // body that died flagged (the server's word killed a puppet) is
            // released through the promote's dead branch (PuppetApi.cpp
            // Promote: the block lifted, the AI left off, the flag cleared) and
            // never demoted again; the deferred kill above keeps holding it for
            // the server's word, and the fate's resurrect (FateApi.cpp, the AI
            // on for the call) stands it up as a live actor, which the next
            // pass makes a puppet as before.
            if (flagged) {
                this.sp.setPuppet(localId, false);
                this.puppets.delete(localId);
                logTrace(this, `body ${localId.toString(16)}: released from the puppet state, its activation the engine's`);
            }
            return;
        }
        if (!has3D) {
            return;
        }
        if (!flagged) {
            if (!this.sp.setPuppet(localId, true)) {
                logError(this, `setPuppet(${localId.toString(16)}, true) found no actor`);
                return;
            }
            this.puppets.add(localId);
            // the engine's activation (Talk, trade, pickpocket) is the host's
            // alone: the native demote blocks it, so nothing Papyrus-backed runs
            // here, at the load's first attaches (train-34's first head: the one
            // Papyrus call this demote made ran into Intuitive Dragon Ride
            // Control's quest start-up half a second later on the same game
            // thread, b-invis-recast on gate 20261023-031111)
            logTrace(this, `puppet ${localId.toString(16)}`);
        }
        if (ac.isDead()) {
            // a corpse carries no ability on the engine either
            this.stopCloakLook(localId, ac);
        } else {
            this.playCloakLook(localId, ac);
        }
    }

    // The poll: every actor in the engine's high-process list (a named
    // resident the engine runs is found here within pollMs of its appearance,
    // the cellAttach events naming temporary references alone) and every
    // actor this service has touched, through reconcile. A 0xff actor the
    // server does not describe (a summon, the engine's own temporaries) is
    // nobody's and left alone; a copy with a view goes through the view's
    // own call as well.
    private onUpdate() {
        if (!this.enabled) {
            return;
        }
        const now = Date.now();
        if (now - this.lastPoll < PuppetService.pollMs) {
            return;
        }
        this.lastPoll = now;
        const ids = new Set<number>(this.sp.getHighProcessActors());
        this.puppets.forEach((id) => ids.add(id));
        this.attached.forEach((id) => ids.add(id));
        ids.forEach((id) => {
            if (id === 0x14) {
                return;
            }
            if (id >= 0xff000000 && !localIdToRemoteId(id)) {
                return;
            }
            this.reconcile(id);
        });
    }

    // Whether this game switched the actor's AI off (by its local form id):
    // the native flag, the detours' truth.
    isPuppet(localId: number): boolean {
        return this.sp.isPuppet(localId);
    }

    // Whether the actor is a player's figure or the player's own clone: a
    // server actor whose model carries no NPC base (the server omits base
    // 0x7, PartOne.cpp) or is the clone. A figure is never a puppet: its
    // own game's reports hold it, and its hold (the keep-offset on itself)
    // is an AI package, so a demote leaves it standing where it spawned
    // (a-racemenu 20261022-050437 on the gate: c2's figure of c1 absent
    // where the server had c1, after c2 demoted it at its attach)
    private isFigure(localId: number): boolean {
        // the engine's own answer first: a figure is placed from the player
        // base 0x7 or from a runtime base the appearance made (0xff and
        // above); a server-made copy of a game-file NPC carries that NPC's
        // base. The view's id map below can lag a copy's cellAttach by a
        // tick, and a figure read as a copy was demoted (a-racemenu
        // 20261022-050437; playtest 31's figures moving oddly for the other
        // seat, 2026-10-22)
        const ac = this.sp.Actor.from(this.sp.Game.getFormEx(localId));
        const base = ac ? ac.getBaseObject() : null;
        if (base) {
            const baseId = base.getFormID();
            if (baseId === 0x7 || baseId >= 0xff000000) {
                return true;
            }
        }
        const remoteId = localIdToRemoteId(localId);
        if (!remoteId) {
            return false;
        }
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const form = world.forms.find((f) => f !== undefined && f.refrId === remoteId);
        if (!form) {
            return false;
        }
        // a player's figure: no base, the player base, or the message's
        // default 0 (every player's CreateActor carries baseId 0, 2026-10-22)
        return form.isMyClone === true || !form.baseId || form.baseId === 0x7;
    }

    // The view's two words of before, kept for their callers: a puppet
    // unless the word says otherwise, live by the word.
    ensurePuppet(refr: ObjectReference): void {
        this.reconcile(refr.getFormID(), false);
    }

    ensureLive(refr: ObjectReference, last?: Movement): void {
        this.reconcile(refr.getFormID(), true, last);
    }

    hostedCount(): number {
        return this.puppets.size;
    }

    // An actor whose form the server took back while it was live here (this
    // game left its reach; a disconnect): no word, so not this game's; a
    // puppet if loaded, released to its schedule after the grace if not.
    demoteOnLoss(localId: number): void {
        this.reconcile(localId, false);
    }

    // A stood-up actor (deathService's resurrect) holds at zero again for
    // the server's word: the deferred kill is re-armed on the next pass.
    onResurrected(localId: number): void {
        this.deferredArmed.delete(localId);
    }

    // thuum docs/verbs/npc-stream.md, playtest 37 finding 60 (the visuals
    // slice): a creature's constant-effect ability is the engine's to apply
    // to its actor, and the watcher's puppet stood without its look while
    // the host's atronach burned (HYPOTHESIS from the finding: the engine
    // applies no ability on an AI-off actor). The look alone is restored, R3:
    // among the abilities the puppet's race and base list, each constant
    // effect on self is played on the puppet for the puppet period by its
    // hit shader (EffectShader.play) and by the VisualEffect wrapping its
    // hit effect art (VisualEffect.play), -1 being Papyrus's until stopped;
    // stopped at the promote; read once per 3D. No spell is added to the
    // puppet: the ability on this engine would burn this game's own player,
    // and the cloak's damage is the server's (PartOne::AbilityReaching).
    // Read from Skyrim.esm (lab/esm.py, 2026-10-28): AbFlameAtronach 0x2f3b5
    // sits on AtronachFlameRace 0x131f5; none of its five effects has a hit
    // shader, and its cloak effect AbAtronachCloakFire 0x64459 has no look at
    // all; the flames are AbFXAtronachFlame 0x60da1's hit effect art.
    private playCloakLook(localId: number, ac: Actor): void {
        if (!this.cloakLook || this.cloakLooks.has(localId)) {
            return;
        }
        const looks = this.abilityLooks(ac);
        const playing: CloakLook[] = [];
        for (const look of looks) {
            const form = this.sp.Game.getFormEx(look.formId);
            const id = localId.toString(16);
            // a copy the engine kept across the 3D's reload would stack a
            // second one; a stop of a look not playing does nothing
            if (look.kind === "shader") {
                const shader = this.sp.EffectShader.from(form);
                if (!shader) {
                    continue;
                }
                shader.stop(ac);
                shader.play(ac, -1);
                logTrace(this, `cloak look: ${id} plays shader ${look.formId.toString(16)} of ability ${look.abilityId.toString(16)}`);
            } else {
                const visual = this.sp.VisualEffect.from(form);
                if (!visual) {
                    continue;
                }
                visual.stop(ac);
                visual.play(ac, -1, null);
                logTrace(this, `cloak look: ${id} plays visual effect ${look.formId.toString(16)} of art ${look.artId.toString(16)} of ability ${look.abilityId.toString(16)}`);
            }
            playing.push(look);
        }
        // an empty list is a read too: the lists are not walked again for
        // this 3D (reconcile runs every update for every viewed actor)
        this.cloakLooks.set(localId, playing);
    }

    private stopCloakLook(localId: number, ac: Actor): void {
        const playing = this.cloakLooks.get(localId);
        this.cloakLooks.delete(localId);
        if (!playing) {
            return;
        }
        for (const look of playing) {
            const form = this.sp.Game.getFormEx(look.formId);
            if (look.kind === "shader") {
                this.sp.EffectShader.from(form)?.stop(ac);
            } else {
                this.sp.VisualEffect.from(form)?.stop(ac);
            }
        }
    }

    // The looks of the actor's abilities: every spell the race's list and
    // the base's hold (a creature's abilities live there; the actor's own
    // added spells are its host engine's and a distributor's), each of its
    // effects that is a constant effect delivered to self (an ability's
    // shape, never a listed combat spell's: MagicSystem::CastingType
    // kConstantEffect 0 and Delivery kSelf 0, CommonLibSSE-NG
    // include/RE/M/MagicSystem.h), by its hit shader and by the VisualEffect
    // wrapping its hit effect art, once per form.
    private abilityLooks(ac: Actor): CloakLook[] {
        const out: CloakLook[] = [];
        const seenForms = new Set<number>();
        const seenSpells = new Set<number>();
        const lists: { getSpellCount(): number; getNthSpell(n: number): Spell | null }[] = [];
        const race = ac.getRace();
        if (race) {
            lists.push(race);
        }
        const base = ac.getLeveledActorBase();
        if (base) {
            lists.push(base);
        }
        for (const list of lists) {
            const count = list.getSpellCount();
            for (let i = 0; i < count; ++i) {
                const spell = list.getNthSpell(i);
                if (!spell) {
                    continue;
                }
                const abilityId = spell.getFormID();
                if (seenSpells.has(abilityId)) {
                    continue;
                }
                seenSpells.add(abilityId);
                const effects = spell.getNumEffects();
                for (let j = 0; j < effects; ++j) {
                    const effect = spell.getNthEffectMagicEffect(j);
                    if (!effect || effect.getCastingType() !== 0 || effect.getDeliveryType() !== 0) {
                        continue;
                    }
                    const shader = effect.getHitShader();
                    if (shader && !seenForms.has(shader.getFormID())) {
                        seenForms.add(shader.getFormID());
                        out.push({ kind: "shader", formId: shader.getFormID(), artId: 0, abilityId });
                    }
                    const art = effect.getHitEffectArt();
                    const visual = art ? PuppetService.artVisualEffects.get(art.getFormID()) : undefined;
                    if (art && visual !== undefined && !seenForms.has(visual)) {
                        seenForms.add(visual);
                        out.push({ kind: "visual", formId: visual, artId: art.getFormID(), abilityId });
                    }
                }
            }
        }
        return out;
    }

    private forget(localId: number) {
        this.puppets.delete(localId);
        this.cloakLooks.delete(localId);
        this.attached.delete(localId);
        this.deferredArmed.delete(localId);
        this.unloadedSince.delete(localId);
    }

    // An actor attaching to a cell is loaded here from this moment, whatever
    // its 3D says (ADR-050 decision 3's "loaded here from the attach"); its
    // state follows from the word.
    private onCellAttach(e: CellAttachDetachEvent) {
        if (!this.enabled || !e.refr) {
            return;
        }
        const ac = this.sp.Actor.from(e.refr);
        if (!ac || ac.getFormID() === 0x14) {
            return;
        }
        const id = ac.getFormID();
        this.attached.add(id);
        this.reconcile(id);
    }

    // A detached reference unloads; its reload is a fresh attach. The flag
    // stays until the grace releases it (an unloaded actor streams nothing
    // from here either way).
    private onCellDetach(e: CellAttachDetachEvent) {
        if (!e.refr) {
            return;
        }
        const id = e.refr.getFormID();
        this.attached.delete(id);
        this.controller.lookupListener(HostService).markHosted(id, false);
        this.reconcile(id);
    }

    // A puppet drafted into combat (a hit, a script's StartCombat; the AI bit
    // gates neither, re-ai-puppet.md:181-187): undone at the first event (H6)
    // by the demote again, which is idempotent and stops the combat natively
    private onCombatState(e: CombatEvent) {
        if (!e.actor || !(e.isCombat || e.isSearching)) {
            return;
        }
        const id = e.actor.getFormID();
        if (!this.sp.isPuppet(id)) {
            return;
        }
        this.sp.setPuppet(id, true);
    }

    // A grant or a release reaches every actor as the server's word per actor
    // (isHostedByOther flipped under its id); each view's update runs
    // reconcile with it, persistent references included. The cell walk that
    // stood here (Cell.getNthRef over the cell's own references) missed them
    // and ran at the message's tick, outside the update
    // (b-npc-puppet 20261022-081631: 5 promoted of 23, "Game.getFormEx can't
    // be called in this context" 27 times).

    private readonly puppets = new Set<number>();
    // the looks playing on each puppet, by local id (an empty list once the
    // lists were read for the current 3D)
    private readonly cloakLooks = new Map<number, CloakLook[]>();
    // The Skyrim Platform API reads an effect's hit effect art
    // (MagicEffect.getHitEffectArt) but plays only a VisualEffect (an RFCT
    // wrapping an art, a shader or both), and nothing maps an art to one; the
    // general path is one native, TESObjectREFR::ApplyArtObject
    // (CommonLibSSE-NG include/RE/T/TESObjectREFR.h:368). Until the platform
    // has it, this table is the map: the vanilla RFCT wrapping each art a
    // creature's ability shows, read from Skyrim.esm with lab/esm.py (every
    // RFCT's DATA scanned, 2026-10-28). Skyrim.esm is always load index 00.
    private static readonly artVisualEffects = new Map<number, number>([
        // FXAtronachFireTrailObject (AbFXAtronachFlame 0x60da1 of
        // AbFlameAtronach 0x2f3b5): FXAtronachFireTrailEffect
        [0x60d9c, 0x60d9d],
    ]);
    private readonly attached = new Set<number>();
    private readonly deferredArmed = new Set<number>();
    private readonly unloadedSince = new Map<number, number>();
    private lastPoll = 0;
    private static readonly pollMs = 2000;
    // two polls: a 3D the late fate dropped comes back within it (train-47b
    // reloads it), and a walk out of reach is a release either way
    private static readonly releaseGraceMs = 4000;
    private readonly enabled: boolean;
    private readonly engineDeaths: boolean;
    private readonly cloakLook: boolean;
}
