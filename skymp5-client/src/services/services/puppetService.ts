import { CellAttachDetachEvent, CombatEvent, ObjectReference } from "skyrimPlatform";
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
export class PuppetService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.enabled = this.sp.settings["skymp5-client"]["npcPuppet"] !== false;
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

        // the deferred kill, once per actor (re-armed after a resurrect):
        // no NPC dies by this game's hand until the server's word
        if (!this.deferredArmed.has(localId)) {
            ac.startDeferredKill();
            this.deferredArmed.add(localId);
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
            this.sp.setPuppet(localId, false);
            if (localId < 0xff000000 && ac.isDead()) {
                // thuum docs/verbs/npc-fate.md (the deaths): a corpse stays the
                // server's container on its new host; the promote lifted the
                // puppet's block, the body's goes back on (deathService puts
                // it on at the kill, Resurrect's path takes it off)
                ac.blockActivation(true);
            }
            this.puppets.delete(localId);
            logTrace(this, `live ${localId.toString(16)}`);
            return;
        }

        // not this game's to simulate: a puppet, once it has a 3D (an actor
        // still loading is left alone until it has one; the view asks again
        // every update: the gate's hang at a landing, 2026-10-21, under
        // reading: the demote must not race the loader on a half-made actor)
        host.markHosted(localId, false);
        if (flagged || !has3D) {
            return;
        }
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

    private forget(localId: number) {
        this.puppets.delete(localId);
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
    private readonly attached = new Set<number>();
    private readonly deferredArmed = new Set<number>();
    private readonly unloadedSince = new Map<number, number>();
    private lastPoll = 0;
    private static readonly pollMs = 2000;
    // two polls: a 3D the late fate dropped comes back within it (train-47b
    // reloads it), and a walk out of reach is a release either way
    private static readonly releaseGraceMs = 4000;
    private readonly enabled: boolean;
}
