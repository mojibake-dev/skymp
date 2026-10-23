import { CellAttachDetachEvent, CellFullyLoadedEvent, ObjectReference } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { HostGrantMessage } from "../messages/hostGrantMessage";
import { HostReleaseMessage } from "../messages/hostReleaseMessage";
import { ActorSightingsMessage } from "../messages/actorSightingsMessage";
import { FormModel } from "../../view/model";
import { localIdToRemoteId, remoteIdToLocalId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";

// thuum docs/verbs/cell-host.md (ADR-035): one player's game runs a cell's
// NPCs and the server decides which. This game reports its loaded set, one
// CellLoaded per cell as the engine fully loads it (cellFullyLoaded) and as
// it leaves the loaded set (Cell.isAttached read once a second); the server
// elects from those reports and its own grid, and tells this game with
// HostGrant and HostRelease, each carrying the cell's epoch. The hosted set
// here is what the other services ask: isHostedHere(refr), and isMine(form,
// refr) for an actor this game simulates and reports, where the grant and
// the server's own word on the actor (isHostedByOther false, flipped at the
// grant) agree. A grant whose epoch is not above the last seen for the cell
// in this connection is ignored (a late grant after a release); a release
// for a grant this game does not hold is ignored too; both clear when the
// player's own actor is created again (a reconnect, a restarted server
// counting from 1), when the loaded set is reported anew. The per-actor
// claim of old (Host 14, tryHost) is gone: the server ignores it.
export class HostService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.enabled = this.sp.settings["skymp5-client"]["cellHost"] !== false;
        if (!this.enabled) {
            logTrace(this, "cellHost is off in settings: no loaded set is reported, the server grants nothing");
        }
        this.controller.on("cellFullyLoaded", (e) => this.onCellFullyLoaded(e));
        this.controller.on("update", () => this.onUpdate());
        this.controller.emitter.on("hostGrantMessage", (e) => this.onHostGrant(e));
        this.controller.emitter.on("hostReleaseMessage", (e) => this.onHostRelease(e));
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        // the engine's actors in reach, for the roster (docs/verbs/npc-puppet.md,
        // ActorSightings): an actor the server does not hold is reported to it
        // while this game hosts the cell the actor stands in
        this.controller.on("cellAttach", (e) => this.onActorAttach(e));
        this.controller.on("cellDetach", (e) => this.onActorDetach(e));
    }

    // Whether this game hosts the cell the reference stands in.
    isHostedHere(refr: ObjectReference | null | undefined): boolean {
        if (!refr) {
            return false;
        }
        const cell = refr.getParentCell();
        return cell !== null && this.hosted.has(cell.getFormID());
    }

    // Whether the actor behind `form` is this game's to simulate and report.
    // The server's word per actor (thuum docs/verbs/npc-puppet.md, ADR-045
    // point 2 as revised 2026-10-22): isHostedByOther false names the one
    // game that simulates the actor, a server-made 0xff copy and an actor
    // from the game files under its own id alike; the server derives it from
    // the cell's grant. The cell is not asked here: for a persistent
    // reference getParentCell is the worldspace's persistent cell, so
    // Riverwood's named residents never counted as their host's
    // (b-npc-puppet 20261022-081631). A player's figure is never false, and
    // the player's own actor is nobody's puppet.
    isMine(form: FormModel | undefined, refr: ObjectReference | null | undefined): boolean {
        if (!form || !refr) {
            return false;
        }
        if (refr.getFormID() === 0x14) {
            return false;
        }
        return form.isHostedByOther === false;
    }

    // The same for an actor by its local form id (a hit's aggressor, a
    // cast's caster): its form in the world model and its reference.
    isMineLocal(localId: number): boolean {
        const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(localId));
        const remoteId = localIdToRemoteId(localId);
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const form = world.forms.find((f) => f !== undefined && f.refrId === remoteId);
        return this.isMine(form, refr);
    }

    // The remote ids of the actors this game simulates, for the input
    // stream (sendInputsService): isMine's rule over the world model, the
    // player's own aside (an actor from the game files travels under its
    // own id, so remote and local ids agree for it).
    hostedActorRemoteIds(): number[] {
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const out: number[] = [];
        world.forms.forEach((form, i) => {
            if (form === undefined || i === world.playerCharacterFormIdx) {
                return;
            }
            const remoteId = form.refrId;
            if (remoteId === undefined) {
                return;
            }
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(remoteIdToLocalId(remoteId)));
            if (refr && this.sp.Actor.from(refr) && this.isMine(form, refr)) {
                out.push(remoteId);
            }
        });
        return out;
    }

    // The cells this game hosts, with their epochs.
    hostedCells(): ReadonlyMap<number, number> {
        return this.hosted;
    }

    private onCellFullyLoaded(e: CellFullyLoadedEvent) {
        const id = e.cell.getFormID();
        if (this.loaded.has(id)) {
            return;
        }
        this.loaded.add(id);
        this.report(id, true);
    }

    // Once a second: a cell the engine detached since (or whose form is gone)
    // leaves the loaded set and is reported so. Every refreshMs the whole
    // loaded set is reported again: a report is a bit the server sets, so a
    // repeat costs nothing there, and it heals a report the server never
    // took (b-cell-host 20261021-184501: a teleport's burst ran past the
    // budget and the landing cell's own report was refused, never retried,
    // and the cell went unhosted for the run; the budget is sized for the
    // burst since, and this is the second line).
    private onUpdate() {
        const now = Date.now();
        if (now - this.lastPoll < HostService.pollMs) {
            return;
        }
        this.lastPoll = now;
        this.reportSightings(now);
        for (const id of Array.from(this.loaded)) {
            const cell = this.sp.Cell.from(this.sp.Game.getFormEx(id));
            if (cell === null || !cell.isAttached()) {
                this.loaded.delete(id);
                this.report(id, false);
            }
        }
        // the player's own cell, without waiting for an event: at a teleport
        // the engine's cellFullyLoaded for the destination cell itself can go
        // missing while its neighbours' arrive (x-npc-legs 20261022-211702:
        // c1 reported 9752, 9733, 9712 and 9731 and never 9732, the landing's
        // own, so Riverwood's centre went unhosted for the run; the 30 s
        // refresh re-reports only what this set holds). The player's parent
        // cell is the engine's answer for the player, interior or exterior
        const player = this.sp.Game.getPlayer();
        const own = player ? player.getParentCell() : null;
        if (own && own.isAttached() && !this.loaded.has(own.getFormID())) {
            const id = own.getFormID();
            this.loaded.add(id);
            this.report(id, true);
            logTrace(this, `own cell ${id.toString(16)} reported without its load event`);
        }
        if (now - this.lastRefresh >= HostService.refreshMs) {
            this.lastRefresh = now;
            for (const id of Array.from(this.loaded)) {
                this.report(id, true);
            }
        }
    }

    private onHostGrant(e: ConnectionMessage<HostGrantMessage>) {
        const { cell, epoch } = e.message;
        const last = this.lastEpoch.get(cell) ?? 0;
        if (epoch <= last) {
            logTrace(this, `grant for cell ${cell.toString(16)} at epoch ${epoch} ignored: epoch ${last} seen`);
            return;
        }
        this.lastEpoch.set(cell, epoch);
        this.hosted.set(cell, epoch);
        logTrace(this, `hosting cell ${cell.toString(16)} at epoch ${epoch}`);
        // the cell's puppets go live (PuppetService, docs/verbs/npc-puppet.md)
        this.controller.emitter.emit("cellHostChanged", { cell, hosted: true });
    }

    private onHostRelease(e: ConnectionMessage<HostReleaseMessage>) {
        const { cell, epoch } = e.message;
        const held = this.hosted.get(cell);
        if (held === undefined || held !== epoch) {
            logTrace(this, `release for cell ${cell.toString(16)} at epoch ${epoch} ignored: ${held === undefined ? "not hosted here" : `epoch ${held} held`}`);
            return;
        }
        this.hosted.delete(cell);
        if (epoch > (this.lastEpoch.get(cell) ?? 0)) {
            this.lastEpoch.set(cell, epoch);
        }
        logTrace(this, `released cell ${cell.toString(16)} (epoch ${epoch})`);
        // the cell's actors become puppets (PuppetService)
        this.controller.emitter.emit("cellHostChanged", { cell, hosted: false });
    }

    // The player's own actor is created: a fresh table on the server's side
    // of this connection (a login, a reconnect, a restart), so the epochs
    // clear and the loaded set is reported anew, in its own order
    private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
        if (!e.message.isMe) {
            return;
        }
        for (const cell of Array.from(this.hosted.keys())) {
            this.controller.emitter.emit("cellHostChanged", { cell, hosted: false });
        }
        this.hosted.clear();
        this.lastEpoch.clear();
        this.lastRefresh = Date.now();
        for (const id of Array.from(this.loaded)) {
            this.report(id, true);
        }
    }

    private onActorAttach(e: CellAttachDetachEvent) {
        if (!e.refr) {
            return;
        }
        const id = e.refr.getFormID();
        if (id === 0x14 || id >= 0xff000000 || !this.sp.Actor.from(e.refr)) {
            return;
        }
        this.attached.set(id, true);
    }

    private onActorDetach(e: CellAttachDetachEvent) {
        if (e.refr) {
            this.attached.delete(e.refr.getFormID());
        }
    }

    // The roster (thuum docs/verbs/npc-puppet.md, ADR-045's revision of
    // 2026-10-22; TES3MP's actor list): every sightingsMs, the game-file
    // actors in the engine's high-process list (getHighProcessActors, the
    // game thread's snapshot; the cellAttach events named temporary
    // references alone, never a named resident: playtest 33, 2026-10-23)
    // that the server holds no record of (no form model), with where the
    // engine has them. The server loads
    // the record on demand (a village's residents are placed in their
    // houses and walked outside by their schedules, so their placement is
    // not where they stand) and places it there, if this game hosts the
    // cell of that position; its CreateActor and word follow, and from
    // then on the actor is the host's to run and report. Only while this
    // game hosts anything, and only while unknown actors remain.
    private reportSightings(now: number) {
        if (!this.enabled || this.hosted.size === 0 || now - this.lastSightings < HostService.sightingsMs) {
            return;
        }
        this.lastSightings = now;
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const known = new Set<number>();
        for (const form of world.forms) {
            if (form && form.refrId !== undefined && form.refrId < 0xff000000) {
                known.add(form.refrId);
            }
        }
        const sightings: ActorSightingsMessage["sightings"] = [];
        const gone: number[] = [];
        let unknown = 0;
        const high = this.sp.getHighProcessActors();
        high.forEach((id) => {
            if (id === 0x14 || id >= 0xff000000 || known.has(id)) {
                return;
            }
            unknown += 1;
            if (sightings.length >= HostService.sightingsMax) {
                return;
            }
            // the reference is looked up afresh: an event's wrapper is not
            // kept across ticks
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
            if (!refr) {
                gone.push(id);
                return;
            }
            // a reference the engine holds disabled stands nowhere: not sighted
            // (the server would refuse an initially disabled placement anyway,
            // WorldState::AttachEspmRecord; a runtime disable is this game's)
            if (refr.isDisabled()) {
                return;
            }
            const worldSpace = refr.getWorldSpace();
            const worldOrCell = worldSpace ? worldSpace.getFormID() : (refr.getParentCell()?.getFormID() ?? 0);
            if (!worldOrCell) {
                return;
            }
            sightings.push({
                refrId: id,
                worldOrCell,
                pos: [refr.getPositionX(), refr.getPositionY(), refr.getPositionZ()],
                rot: [refr.getAngleX(), refr.getAngleY(), refr.getAngleZ()],
            });
        });
        for (const id of gone) {
            this.attached.delete(id);
        }
        if (sightings.length === 0) {
            return;
        }
        logTrace(this, `roster: ${sightings.length} sighting(s) sent, ${unknown} unknown of ${high.length} high-process, ${this.hosted.size} cell(s) hosted; first ${sightings[0].refrId.toString(16)} in ${sightings[0].worldOrCell.toString(16)}`);
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.ActorSightings, sightings },
            reliability: "reliable",
        });
    }

    private report(cell: number, loaded: boolean) {
        if (!this.enabled) {
            return;
        }
        if (cell === 0) {
            logError(this, "a cell of 0 is nothing to report");
            return;
        }
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.CellLoaded, cell, loaded },
            reliability: "reliable",
        });
    }

    private static readonly pollMs = 1000;
    private static readonly refreshMs = 30_000;
    private static readonly sightingsMs = 2000;
    private static readonly sightingsMax = 64;
    private readonly enabled: boolean;
    private readonly loaded = new Set<number>();
    private readonly hosted = new Map<number, number>();
    private readonly lastEpoch = new Map<number, number>();
    private lastPoll = 0;
    private lastRefresh = 0;
    private lastSightings = 0;
    private readonly attached = new Map<number, true>();
}
