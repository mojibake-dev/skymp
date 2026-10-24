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
// under the engine. The switch is the cell's grant alone: the view asks
// ensurePuppet or ensureLive on every update (idempotent), HostService's
// grant and release flip a whole cell at once, and an actor attaching to a
// cell this game does not host is a puppet from its first frame (TES3MP's
// shape: AI off everywhere but the cells this game hosts). The combat sink
// undoes a draft into combat (a hit or a script; H6). The switch is the
// fork's Skyrim Platform export setPuppet: Q4's demote and promote in one
// native call on the game thread, and the flag the two detours read.
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

    // The demote poll (playtest 33, 2026-10-23): the cellAttach events name
    // temporary references alone, so a named resident the engine runs is
    // found here, in its high-process list (getHighProcessActors), within
    // pollMs of its appearance, and is a puppet unless the server's word
    // names this game as its simulator; the view confirms when the word
    // arrives (a resident the server does not hold yet is a puppet on its
    // own host for one roster round, until its CreateActor's word promotes
    // it). The roster (HostService) reads the same list.
    private onUpdate() {
        if (!this.enabled) {
            return;
        }
        const now = Date.now();
        if (now - this.lastPoll < PuppetService.pollMs) {
            return;
        }
        this.lastPoll = now;
        const host = this.controller.lookupListener(HostService);
        for (const id of this.sp.getHighProcessActors()) {
            if (id === 0x14 || id >= 0xff000000 || this.puppets.has(id)) {
                continue;
            }
            if (host.isMineLocal(id)) {
                continue;
            }
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
            if (refr) {
                this.ensurePuppet(refr);
            }
        }
        // thuum docs/verbs/npc-handoff.md D11 (b-npc-handoff-night 092208,
        // 114904): a puppet this engine no longer loads (the player walked
        // away, the server's Teleport moved it into a house this engine has
        // not attached, a cell detached) is simulated by nobody, and no word
        // reaches a record nobody subscribes; AI off, it would never run its
        // schedule, never be sighted, never come out. Released: the flag
        // off, the AI and activation back on, left to the engine's own
        // schedule; this poll makes it a puppet again within pollMs when it
        // loads here and this seat does not host it
        for (const id of Array.from(this.puppets)) {
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
            if (refr && !refr.is3DLoaded()) {
                this.sp.releasePuppet(id);
                this.puppets.delete(id);
                logTrace(this, `released ${id.toString(16)}: no 3D here, left to its schedule (npc-handoff D11)`);
            }
        }
    }

    // Whether this game switched the actor's AI off (by its local form id).
    isPuppet(localId: number): boolean {
        return this.puppets.has(localId);
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

    // An NPC this game does not host, seen by its view: a puppet from now on.
    ensurePuppet(refr: ObjectReference): void {
        if (!this.enabled) {
            return;
        }
        const id = refr.getFormID();
        if (id === 0x14 || this.puppets.has(id) || this.isFigure(id)) {
            return;
        }
        const ac = this.sp.Actor.from(refr);
        // an actor still loading (no 3D yet: a cell's attach, a copy's spawn)
        // is left alone until it has one; the view asks again every update
        // and cellAttach's call is followed by the view's (the gate's hang at
        // a landing, 2026-10-21, under reading: the demote must not race the
        // loader on a half-made actor)
        if (!ac || !ac.is3DLoaded()) {
            return;
        }
        if (!this.sp.setPuppet(id, true)) {
            logError(this, `setPuppet(${id.toString(16)}, true) found no actor`);
            return;
        }
        this.puppets.add(id);
        // the engine's activation (Talk, trade, pickpocket) is the host's
        // alone: the puppet apply blocks it on the first relayed sample and
        // every one after (movementApply.ts), so nothing Papyrus-backed runs
        // here, at the load's first attaches: the one call this demote made
        // on train-34's first head was the only Papyrus work the client added
        // to that moment, and Intuitive Dragon Ride Control's quest start-up,
        // half a second later on the same game thread, ran its init twice
        // interleaved and showed its "SKSE is not loaded" box in two of two
        // b-invis-recast runs (gate 20261023-031111), against none before
        logTrace(this, `puppet ${id.toString(16)}`);
    }

    // An NPC this game hosts, seen by its view: live from now on, placed at
    // the record's last point first (the relay stops at the grant).
    ensureLive(refr: ObjectReference, last?: Movement): void {
        const id = refr.getFormID();
        if (!this.puppets.has(id)) {
            return;
        }
        const ac = this.sp.Actor.from(refr);
        if (ac && last) {
            // the record's last point and heading first: the relay stops at
            // the grant (the native promote leaves the transform alone)
            ac.setPosition(last.pos[0], last.pos[1], last.pos[2]);
            ac.setAngle(last.rot[0], last.rot[1], last.rot[2]);
        }
        if (ac) {
            // the puppet's imposed state undone, the inverse of the apply's
            // (movementApply.ts: head tracking on the relayed look-at point);
            // the activation block is the native demote's and the native
            // promote lifts it (PuppetApi.cpp; playtest 32's dead Talk prompts
            // after a handoff, playtest 33's dialogue stuck on an unblocked
            // puppet)
            ac.clearLookAt();
            ac.setHeadTracking(true);
        }
        this.sp.setPuppet(id, false);
        if (ac && id < 0xff000000 && ac.isDead()) {
            // thuum docs/verbs/npc-fate.md (the deaths): a corpse stays the
            // server's container on its new host; the promote lifted the
            // puppet's block, the body's goes back on (deathService puts it
            // on at the kill, Resurrect's path takes it off)
            ac.blockActivation(true);
        }
        this.puppets.delete(id);
        logTrace(this, `live ${id.toString(16)}`);
    }

    hostedCount(): number {
        return this.puppets.size;
    }

    // An actor from the game files whose form the server took back while it
    // was live here (this game left its reach; a disconnect): a puppet from
    // now on, loaded or not. The server's word cannot reach a game that no
    // longer listens to the actor, and the release of its cell comes after
    // the grace; the native finds a persistent actor unloaded too and, with
    // no 3D, clears the AI bit alone (PuppetApi.cpp). The view's caller has
    // already told an actor from a figure (a figure is a 0xff copy).
    demoteOnLoss(localId: number): void {
        if (!this.enabled || localId === 0x14 || this.puppets.has(localId)) {
            return;
        }
        // thuum docs/verbs/npc-handoff.md D11 (b-npc-handoff-night 092208 and
        // 114904): an actor with no 3D is simulated by nobody's engine and no
        // word reaches a record nobody subscribes, so it is left to its own
        // schedule: no flag (the detours would skip its movement step once
        // it loads), no set entry (the roster must sight it), no AI change;
        // the server's word rebinds it when its cell loads again
        const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(localId));
        if (refr && !refr.is3DLoaded()) {
            logTrace(this, `${localId.toString(16)} lost with no 3D: left to its schedule (npc-handoff D11)`);
            return;
        }
        if (!this.sp.setPuppet(localId, true)) {
            return;
        }
        this.puppets.add(localId);
        logTrace(this, `puppet ${localId.toString(16)} (its form lost while live)`);
    }

    // An actor attaching to a cell: a puppet unless the server's word already
    // names this game as its simulator (the player's own actor aside); an
    // actor the server has not described yet is a puppet until it does. Its
    // view confirms on the next update.
    private onCellAttach(e: CellAttachDetachEvent) {
        if (!this.enabled || !e.refr) {
            return;
        }
        const ac = this.sp.Actor.from(e.refr);
        if (!ac || ac.getFormID() === 0x14) {
            return;
        }
        if (!this.controller.lookupListener(HostService).isMineLocal(ac.getFormID())) {
            this.ensurePuppet(e.refr);
        }
    }

    // A detached reference unloads; its reload is a fresh attach.
    private onCellDetach(e: CellAttachDetachEvent) {
        if (!e.refr) {
            return;
        }
        this.puppets.delete(e.refr.getFormID());
    }

    // A puppet drafted into combat (a hit, a script's StartCombat; the AI bit
    // gates neither, re-ai-puppet.md:181-187): undone at the first event (H6)
    private onCombatState(e: CombatEvent) {
        if (!e.actor || !(e.isCombat || e.isSearching)) {
            return;
        }
        const id = e.actor.getFormID();
        if (!this.puppets.has(id)) {
            return;
        }
        // the demote again: idempotent, and it stops the combat natively
        this.sp.setPuppet(id, true);
    }

    // A grant or a release reaches every actor as the server's word per actor
    // (isHostedByOther flipped under its id); each view promotes or demotes
    // on its next update, persistent references included. The cell walk that
    // stood here (Cell.getNthRef over the cell's own references) missed them
    // and ran at the message's tick, outside the update
    // (b-npc-puppet 20261022-081631: 5 promoted of 23, "Game.getFormEx can't
    // be called in this context" 27 times).

    private readonly puppets = new Set<number>();
    private lastPoll = 0;
    private static readonly pollMs = 2000;
    private readonly enabled: boolean;
}
