import { CellFullyLoadedEvent, ObjectReference } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { HostGrantMessage } from "../messages/hostGrantMessage";
import { HostReleaseMessage } from "../messages/hostReleaseMessage";
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
    // An actor from the game files (the engine's own reference, its own id
    // under 0xff000000; thuum docs/verbs/npc-puppet.md, ADR-045): the grant
    // alone, since the server sends it no per-actor property (the flip under
    // its u32 id never reached the client's copy: playtest thirty's wolves).
    // A server-made 0xff actor: the grant and the server's word agreeing
    // (isHostedByOther false; a player's figure is never false).
    isMine(form: FormModel | undefined, refr: ObjectReference | null | undefined): boolean {
        if (!form || !refr) {
            return false;
        }
        const id = refr.getFormID();
        if (id < 0xff000000) {
            return id !== 0x14 && this.isHostedHere(refr);
        }
        return form.isHostedByOther === false && this.isHostedHere(refr);
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
        for (const id of Array.from(this.loaded)) {
            const cell = this.sp.Cell.from(this.sp.Game.getFormEx(id));
            if (cell === null || !cell.isAttached()) {
                this.loaded.delete(id);
                this.report(id, false);
            }
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
    private readonly enabled: boolean;
    private readonly loaded = new Set<number>();
    private readonly hosted = new Map<number, number>();
    private readonly lastEpoch = new Map<number, number>();
    private lastPoll = 0;
    private lastRefresh = 0;
}
