import { Actor, CellAttachDetachEvent, CombatEvent, FormType, ObjectReference } from "skyrimPlatform";
import { logTrace } from "../../logging";
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
// undoes a draft into combat (a hit or a script; H6). Until Stage 2's native
// export (setPuppet: Q4's demote and promote in one call, and the flag the
// two detours read), the switch is the Papyrus-level set below.
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
        this.controller.emitter.on("cellHostChanged", (e) => this.onCellHostChanged(e.cell, e.hosted));
    }

    // Whether this game switched the actor's AI off (by its local form id).
    isPuppet(localId: number): boolean {
        return this.puppets.has(localId);
    }

    // An NPC this game does not host, seen by its view: a puppet from now on.
    ensurePuppet(refr: ObjectReference): void {
        if (!this.enabled) {
            return;
        }
        const id = refr.getFormID();
        if (id === 0x14 || this.puppets.has(id)) {
            return;
        }
        const ac = this.sp.Actor.from(refr);
        if (!ac) {
            return;
        }
        this.demote(ac);
        this.puppets.add(id);
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
        if (!ac) {
            this.puppets.delete(id);
            return;
        }
        this.promote(ac, last);
        this.puppets.delete(id);
        logTrace(this, `live ${id.toString(16)}`);
    }

    hostedCount(): number {
        return this.puppets.size;
    }

    // An actor attaching to a cell: a puppet unless this game hosts the cell
    // (the player's own actor aside). Its view, if the server knows it,
    // confirms on the next update.
    private onCellAttach(e: CellAttachDetachEvent) {
        if (!this.enabled || !e.refr) {
            return;
        }
        const ac = this.sp.Actor.from(e.refr);
        if (!ac || ac.getFormID() === 0x14) {
            return;
        }
        if (!this.controller.lookupListener(HostService).isHostedHere(e.refr)) {
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
        this.sp.Actor.from(e.actor)?.stopCombat();
    }

    // The whole cell at a grant or a release: every actor of the cell flips,
    // the server's (with the record's last point) and the engine's own alike
    private onCellHostChanged(cellId: number, hosted: boolean) {
        if (!this.enabled) {
            return;
        }
        const cell = this.sp.Cell.from(this.sp.Game.getFormEx(cellId));
        if (!cell) {
            return;
        }
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const n = cell.getNumRefs(FormType.Character);
        for (let i = 0; i < n; ++i) {
            const refr = cell.getNthRef(i, FormType.Character);
            if (!refr || refr.getFormID() === 0x14) {
                continue;
            }
            if (hosted) {
                const remoteId = localIdToRemoteId(refr.getFormID());
                const form = world.forms.find((f) => f !== undefined && f.refrId === remoteId);
                this.ensureLive(refr, form?.movement);
            } else {
                this.ensurePuppet(refr);
            }
        }
    }

    // Q4's demote (docs/research/m3/re-ai-puppet.md): combat off, the
    // keep-offset gone, the AI bit cleared (the engine zeroes the character
    // controller's velocity with it)
    private demote(ac: Actor) {
        ac.stopCombat();
        ac.clearKeepOffsetFromActor();
        ac.enableAI(false);
    }

    // Q4's promote: the record's last point and heading, the AI bit set, the
    // package evaluated (immediate in sit states 0, 3 and 7, else the first
    // AI tick)
    private promote(ac: Actor, last?: Movement) {
        if (last) {
            ac.setPosition(last.pos[0], last.pos[1], last.pos[2]);
            ac.setAngle(last.rot[0], last.rot[1], last.rot[2]);
        }
        ac.enableAI(true);
        ac.evaluatePackage();
    }

    private readonly puppets = new Set<number>();
    private readonly enabled: boolean;
}
