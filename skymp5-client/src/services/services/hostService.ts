import { Actor, Cell, CellAttachDetachEvent, CellFullyLoadedEvent, Game, ObjectReference, TESModPlatform, WorldSpace } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { HostGrantMessage } from "../messages/hostGrantMessage";
import { HostReleaseMessage } from "../messages/hostReleaseMessage";
import { ActorSightingsMessage } from "../messages/actorSightingsMessage";
import { FormModel } from "../../view/model";
import { Movement } from "../../sync/movement";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { localIdToRemoteId, remoteIdToLocalId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { LoginCellService } from "./loginCellService";
import { RefStateService } from "./refStateService";
import { RemoteServer } from "./remoteServer";
import { ScriptStateService } from "./scriptStateService";

// one actor's hold: the record to stand at, the word's arrival, the asks made
// (the gate's move counts as the first), the checks run and the reports
// deferred past it
type PendingPlacement = { since: number; record: Movement; asks: number; askedAt: number; checks: number; deferred: Array<() => void>; dead: boolean; loadedAt: number };
// deferred reports released by a hold run on the next update, never inline
// (a release from the tick thread, where natives are refused: numidium's
// review of r3, item 1)
type Released = { remoteId: number; deferred: Array<() => void> };

// The storage key of the state a plugin reload must not lose (sp.storage
// survives one, ADR-041)
const HOST_STORAGE_KEY = "hostServiceState";

// thuum docs/verbs/cell-host.md (ADR-035): one player's game runs a cell's
// NPCs and the server decides which. This game reports its loaded set, one
// CellLoaded per cell as the engine fully loads it (cellFullyLoaded, or the
// once-a-second read of a recently detached cell standing attached again,
// since a re-attach from the engine's buffer fires no event) and as
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
        // thuum docs/verbs/npc-stream.md (ADR-050): the stream's switch (a
        // bisect switch, not a play mode): off, the hosted set stays empty,
        // the hook reports the player alone and the host sends movement and
        // animation as before
        this.streamEnabled = this.sp.settings["skymp5-client"]["npcStream"] !== false;
        if (!this.streamEnabled) {
            logTrace(this, "npcStream is off in settings: no hosted actor's actions are streamed");
        }
        this.controller.on("cellFullyLoaded", (e) => this.onCellFullyLoaded(e));
        this.controller.on("update", () => this.onUpdate());
        this.controller.emitter.on("hostGrantMessage", (e) => this.onHostGrant(e));
        this.controller.emitter.on("hostReleaseMessage", (e) => this.onHostRelease(e));
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        // the engine's actors in reach, for the roster (docs/verbs/npc-puppet.md,
        // ActorSightings): an actor the server does not hold is reported to it
        // while this game hosts the cell the actor stands in
        this.controller.on("update", () => this.checkPlacements());
        this.controller.on("cellAttach", (e) => this.onActorAttach(e));
        this.controller.on("cellDetach", (e) => this.onActorDetach(e));
        this.restore();
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
            // docs/engine/non-owning-seat.md P8: a promoted actor's reports
            // wait until its copy STANDS at the record (checkPlacements), so
            // the first report never carries the engine's stale position
            // (065955, 070357: the day copy at the mill reported 88 ms after
            // the gate's decision and before its move landed)
            if (this.pendingPlacement.has(remoteId)) {
                return;
            }
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(remoteIdToLocalId(remoteId)));
            if (refr && this.sp.Actor.from(refr) && this.isMine(form, refr)) {
                out.push(remoteId);
            }
        });
        return out;
    }

    // thuum docs/verbs/npc-stream.md (ADR-050): the hosted set in the fork's
    // Skyrim Platform mirrors the word per actor (setHosted), so the hook on
    // the engine's action entry reports a hosted actor's actions beside the
    // player's; PuppetService calls this at every flip (live, puppet, lost,
    // unloaded). Idempotent: a native call on a change alone.
    markHosted(localId: number, hosted: boolean): void {
        if (!this.streamEnabled || localId === 0x14 || hosted === this.hostedActors.has(localId)) {
            return;
        }
        if (hosted) {
            this.hostedActors.add(localId);
        } else {
            this.hostedActors.delete(localId);
        }
        if (!this.sp.setHosted(localId, hosted)) {
            logError(this, `setHosted(${localId.toString(16)}, ${hosted}) refused`);
        }
    }

    isStreamEnabled(): boolean {
        return this.streamEnabled;
    }

    // thuum docs/DECISIONS.md ADR-059 decision 1: the platform's event block
    // admits a host-scoped script's events only to references in a cell this
    // game hosts; the hosted set is told to it at every grant, release, clear
    // and restore, on the next update (a message handler may call no native).
    // A platform without the native (an older dist) throws: harmless, logged
    // once. `now` from an update's own context (a grant's gate, openGate)
    private tellPlatform(cell: number, hosted: boolean, now = false) {
        const tell = () => {
            try {
                this.sp.callNative("TESModPlatform", "SetCellHosted", undefined, cell, hosted);
            } catch (e) {
                if (!this.platformToldOnce) {
                    this.platformToldOnce = true;
                    logTrace(this, `SetCellHosted unavailable on this platform: ${e}`);
                }
            }
        };
        if (now) {
            tell();
        } else {
            this.controller.once("update", tell);
        }
    }
    private platformToldOnce = false;

    // thuum docs/DECISIONS.md ADR-062 decision 1: the engine's own activation
    // of the cell's scripted world objects (activators and movable statics) is
    // lifted on the host at the grant and blocked again at the release, so a
    // plate's activation parents fire its spike wall and a lever's its
    // portcullis on the hosting seat alone (objectReferenceEx.dealWithRef
    // reads the same rule at a reference's creation). The pass walks the
    // cell's references by kind (Cell.getNumRefs and getNthRef, Papyrus's
    // own), on the next update (natives need the game thread). A trap family
    // reference is passed over both ways (ADR-068 D-S3 point 1: every seat
    // fires the family itself, so its activation is never lifted here nor
    // blocked again; objectReferenceEx.dealWithRef keeps the same rule).
    // `now` from an update's own context, as tellPlatform's
    private liftCellActivation(cell: number, hosted: boolean, now = false) {
        const lift = () => {
            const form = this.sp.Game.getFormEx(cell);
            const cellForm = form ? this.sp.Cell.from(form) : null;
            if (!cellForm) {
                return;
            }
            let touched = 0;
            let traps = 0;
            for (const kind of [this.sp.FormType.Activator, this.sp.FormType.MovableStatic]) {
                const n = cellForm.getNumRefs(kind);
                for (let i = 0; i < n; i++) {
                    const ref = cellForm.getNthRef(i, kind);
                    if (!ref) {
                        continue;
                    }
                    if (ObjectReferenceEx.isTrapFamily(ref.getFormID())) {
                        traps++;
                        continue;
                    }
                    ref.blockActivation(!hosted);
                    touched++;
                }
            }
            logTrace(this, `cell ${cell.toString(16)}: the engine's activation ${hosted ? "lifted" : "blocked"} on ${touched} scripted world object(s) (ADR-062)${traps > 0 ? `, ${traps} trap reference(s) left to the engine (D-S3)` : ""}`);
        };
        if (now) {
            lift();
        } else {
            this.controller.once("update", lift);
        }
    }

    // a grant's gate, from ScriptStateService's pass in an update's context
    // (thuum ADR-068 rung 3, the script record): the platform's hosted set
    // and the activation lift at once, after the cell's script records are
    // applied; a grant released or replaced meanwhile opens nothing (its
    // release told the platform already). Rung 4 (amendment B): the cell's
    // held ref state words go on first, inside the same pass, so the new
    // host's engine starts from the record (RefStateService)
    private openGate(cell: number, epoch: number) {
        if (this.hosted.get(cell) !== epoch) {
            logTrace(this, `cell ${cell.toString(16)}: the gate of the grant at epoch ${epoch} stays shut, the grant is gone`);
            return;
        }
        this.controller.lookupListener(RefStateService).applyAtGrant(cell);
        this.tellPlatform(cell, true, true);
        this.liftCellActivation(cell, true, true);
    }

    // The cells this game hosts, with their epochs.
    hostedCells(): ReadonlyMap<number, number> {
        return this.hosted;
    }

    // Whether this game hosts the cell, by its id (thuum docs/verbs/
    // prop-physics.md: a prop nobody's word names is the cell's host's)
    hostsCell(cellId: number): boolean {
        return cellId !== 0 && this.hosted.has(cellId);
    }

    // thuum docs/verbs/change-form.md (ADR-065, Ownership): the epoch a
    // report of an actor's record carries, the host epoch this client holds
    // for the actor's cell (ADR-066's stamp): its parent cell's grant; for a
    // persistent reference, whose parent cell is the worldspace's persistent
    // cell (isMine's note above), the player's own cell's grant when both
    // stand in one worldspace and one 4096-unit grid square (the exterior
    // cell's size, movementApply's cellWidth); 0 when neither names a hosted
    // cell, which the server refuses (E_CHANGEFORM_EPOCH) and the sender's
    // line says
    epochForActor(refr: ObjectReference): number {
        const cell = refr.getParentCell();
        const own = cell ? this.hosted.get(cell.getFormID()) : undefined;
        if (own !== undefined) {
            return own;
        }
        const player = this.sp.Game.getPlayer();
        const playerCell = player ? player.getParentCell() : null;
        const world = refr.getWorldSpace();
        const playerWorld = player ? player.getWorldSpace() : null;
        if (!player || !playerCell || !world || !playerWorld || world.getFormID() !== playerWorld.getFormID()) {
            return 0;
        }
        const sameSquare = Math.floor(refr.getPositionX() / 4096) === Math.floor(player.getPositionX() / 4096)
            && Math.floor(refr.getPositionY() / 4096) === Math.floor(player.getPositionY() / 4096);
        return sameSquare ? (this.hosted.get(playerCell.getFormID()) ?? 0) : 0;
    }

    // docs/engine/non-owning-seat.md P8 and section 3: the word named this
    // seat for `remoteId`, and its reports hold until the engine's copy
    // STANDS at the record (the engine's cell the record's and the position
    // within placedWithinUnits), checked each update. One mover per
    // promotion: the hold (the view's gate defers to a standing hold,
    // formView); the hold asks the move on its second check and asks again
    // every reaskAfterMs while an ask has not landed, up to maxAsks (the last
    // at about 6 s, past a cold login's load block, where MoveRefrToPosition
    // is a no-op for 50 Papyrus updates of 80 ms or more, LoadGame.cpp: at
    // least 4 s, 5 by its own comment); the timeout counts from the last ask,
    // so a late landing is waited for and a stuck one is released with a
    // line naming both positions (numidium's reviews of 0b61fc80 item 1 and
    // of m3-system1 item 3). A body is never moved (D78 C): a dead record is not
    // held and a copy found dead is released unmoved (item 2). A missing form
    // or record releases at once, so no actor is held for good. Reports
    // deferred past the hold (a death) run at its release (item 4).
    holdUntilPlaced(remoteId: number, record: Movement | undefined, deadByTheWord = false): void {
        if (!record) {
            logTrace(this, `hold for ${remoteId.toString(16)}: no record held, nothing to place at`);
            return;
        }
        const live = this.pendingPlacement.get(remoteId);
        // the word's own dead flag (the create word's isDead, the model's), not
        // the movement's, which the create word builds false (review item 2); a
        // dead word ends a live hold unmoved, its deferred reports dropped (the
        // server's word stands)
        if (deadByTheWord || record.isDead) {
            // a dead word: no move ever (D78 C), but the reports hold until the
            // engine's copy IS dead here (formView's bind kill at its first
            // check with 3D) or deadBindTimeoutMs pass, so a fresh process whose
            // engine loaded the body alive never sends an alive sample of a dead
            // record (x-npc-death-reentry v4's run 20261103-100946: the server
            // took such a sample as a resurrection by the host's engine)
            if (live) {
                live.record = record;
                live.dead = true;
                logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: dead at the word, a body is never moved (D78 C); the hold waits for the body to be dead here${live.deferred.length > 0 ? `, ${live.deferred.length} deferred report(s) dropped (the server's word stands)` : ""}`);
                live.deferred = [];
            } else {
                this.pendingPlacement.set(remoteId, { since: Date.now(), record, asks: 0, askedAt: 0, checks: 0, deferred: [], dead: true, loadedAt: 0 });
                logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: dead at the word, a body is never moved (D78 C); the reports wait for the body to be dead here`);
            }
            return;
        }
        if (live) {
            // a re-sent word mid-hold refreshes the record and keeps the hold's
            // deferred reports and its asks (review item 7); a live word on a
            // dead hold turns it back into a placing hold (round six's
            // review, item 6)
            live.record = record;
            if (live.dead) {
                live.dead = false;
                live.since = Date.now();
                live.asks = 0;
                live.askedAt = 0;
                live.checks = 0;
                logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: the word is live again, the hold places`);
            }
            return;
        }
        this.pendingPlacement.set(remoteId, { since: Date.now(), record, asks: 0, askedAt: 0, checks: 0, deferred: [], dead: false, loadedAt: 0 });
    }

    // a demote (the word names another game) or any other end of the
    // reason to hold: the hold ends with its deferred reports run
    releaseHold(remoteId: number, why: string): void {
        const hold = this.pendingPlacement.get(remoteId);
        if (!hold) {
            return;
        }
        this.pendingPlacement.delete(remoteId);
        // a demote: this seat is no longer the actor's reporter, so a death it
        // deferred cannot be reported by it (the server would refuse it,
        // E_DEATH_NOT_HOST); the new host's engine or the server's word carries
        // it, and the drop is said (review item 1)
        logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: released after ${Date.now() - hold.since} ms, ${why}${hold.deferred.length > 0 ? `; ${hold.deferred.length} deferred report(s) dropped with it (not this seat's to send)` : ""}`);
    }

    // the live word (isDead false, the RespawnEvent): a dead hold turns back
    // into a placing hold with the record refreshed, never a bare release (a
    // teleporting respawn lands frames later behind the ragdoll's removal, and
    // reports let loose first would overwrite the spawn point with the
    // corpse's place: numidium's review of round eight, item 4)
    reviveHold(remoteId: number, record: Movement | undefined): void {
        const live = this.pendingPlacement.get(remoteId);
        if (!live || !live.dead) {
            return;
        }
        if (record) {
            live.record = record;
        }
        live.dead = false;
        live.since = Date.now();
        live.asks = 0;
        live.askedAt = 0;
        live.checks = 0;
        logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: the record is alive again (the live word), the hold places`);
    }

    // the senders of a report that carries a position (a death, a shot's or a
    // cast's origin): the word and the placement
    isReportable(localId: number): boolean {
        return this.isMineLocal(localId) && !this.pendingPlacement.has(localIdToRemoteId(localId));
    }

    isHeld(localId: number): boolean {
        return this.pendingPlacement.has(localIdToRemoteId(localId));
    }

    // a report that can wait (a death: the engine's kill stands on this seat
    // and the server's word is late, never lost) runs when the hold ends,
    // placed or not; true when deferred, false when there is no hold
    deferPastHold(localId: number, what: string, run: () => void): boolean {
        const remoteId = localIdToRemoteId(localId);
        const hold = this.pendingPlacement.get(remoteId);
        if (!hold) {
            return false;
        }
        hold.deferred.push(run);
        logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: ${what} deferred past the hold`);
        return true;
    }

    // queued for the next update (never run inline: a release can come from
    // the tick thread, where natives are refused)
    private runDeferred(remoteId: number, deferred: Array<() => void>): void {
        if (deferred.length > 0) {
            this.released.push({ remoteId, deferred });
        }
    }

    private runReleased(): void {
        if (this.released.length === 0) {
            return;
        }
        const batch = this.released;
        this.released = [];
        for (const r of batch) {
            for (const run of r.deferred) {
                try {
                    run();
                } catch (e) {
                    logError(this, `hold ${r.remoteId.toString(16)}: a deferred report threw: ${e}`);
                }
            }
        }
    }

    private checkPlacements(): void {
        this.runReleased();
        if (this.pendingPlacement.size === 0) {
            return;
        }
        const now = Date.now();
        this.pendingPlacement.forEach((hold, remoteId) => {
            // a throw inside one hold's check must not stop the others' or
            // hold this actor for good (item 8)
            try {
                this.checkPlacement(hold, remoteId, now);
            } catch (e) {
                this.pendingPlacement.delete(remoteId);
                logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: the check threw (${e}); released`);
                this.runDeferred(remoteId, hold.deferred);
            }
        });
    }

    private checkPlacement(hold: PendingPlacement, remoteId: number, now: number): void {
        hold.checks += 1;
        const refr = ObjectReference.from(Game.getFormEx(remoteIdToLocalId(remoteId)));
        if (!refr) {
            // a server-made copy's form follows its create word by frames (the
            // client places it); the hold waits for it and releases only when
            // none appears within the whole budget (review item 7)
            if (now - hold.since >= HostService.noFormTimeoutMs) {
                this.pendingPlacement.delete(remoteId);
                logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: no form here after ${now - hold.since} ms, released`);
                this.runDeferred(remoteId, hold.deferred);
            }
            return;
        }
        if (hold.dead) {
            // the bind needs the copy's 3D, so the budget counts from the 3D
            // load, not the word (round six's review, item 5), under a far cap
            const body = Actor.from(refr);
            if (hold.loadedAt === 0 && refr.is3DLoaded()) {
                hold.loadedAt = now;
            }
            if (body && body.isDead()) {
                this.pendingPlacement.delete(remoteId);
                logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: the body is dead here after ${now - hold.since} ms; released`);
                this.runDeferred(remoteId, hold.deferred);
            } else if ((hold.loadedAt !== 0 && now - hold.loadedAt >= HostService.deadBindTimeoutMs) || now - hold.since >= HostService.deadHoldCapMs) {
                this.pendingPlacement.delete(remoteId);
                logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: dead at the word but alive here after ${now - hold.since} ms${hold.loadedAt !== 0 ? ` (${now - hold.loadedAt} ms since its 3D loaded)` : " (its 3D never loaded)"}; released, the server's rules decide`);
                this.runDeferred(remoteId, hold.deferred);
            }
            return;
        }
        const r = hold.record;
        const engineCell = ObjectReferenceEx.getWorldOrCell(refr);
        const ex = refr.getPositionX(), ey = refr.getPositionY(), ez = refr.getPositionZ();
        const dist = Math.sqrt((ex - r.pos[0]) ** 2 + (ey - r.pos[1]) ** 2 + (ez - r.pos[2]) ** 2);
        const engineAt = `(${ex.toFixed(0)}, ${ey.toFixed(0)}, ${ez.toFixed(0)}) in ${engineCell.toString(16)}`;
        const recordAt = `(${r.pos[0].toFixed(0)}, ${r.pos[1].toFixed(0)}, ${r.pos[2].toFixed(0)}) in ${r.worldOrCell.toString(16)}`;
        const asked = hold.asks === 0 ? "never asked" : `asked ${hold.asks} time(s), the last at +${hold.askedAt - hold.since} ms`;
        if (engineCell === r.worldOrCell && dist <= HostService.placedWithinUnits) {
            this.pendingPlacement.delete(remoteId);
            logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: placed at the record after ${now - hold.since} ms (${asked}), the engine at ${engineAt}, the record at ${recordAt}; released`);
            this.runDeferred(remoteId, hold.deferred);
            return;
        }
        const actor = Actor.from(refr);
        if (actor && actor.isDead()) {
            this.pendingPlacement.delete(remoteId);
            logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: dead on this seat after ${now - hold.since} ms, a body is never moved (D78 C), the engine at ${engineAt}, the record at ${recordAt}; released unmoved`);
            this.runDeferred(remoteId, hold.deferred);
            return;
        }
        // the hold is the one mover while it stands (the view's gate defers to
        // it, formView): the first check leaves a frame for the word's apply,
        // the second asks, and an ask that did not land is repeated every
        // reaskAfterMs up to maxAsks (a cold login's first 50 Papyrus updates
        // make the move a no-op, LoadGame.cpp, 4 to 5 s: the load block)
        const askNow = (hold.asks === 0 && hold.checks >= 2) ||
            (hold.asks >= 1 && hold.asks < HostService.maxAsks && now - hold.askedAt >= HostService.reaskAfterMs);
        if (askNow) {
            hold.asks += 1;
            hold.askedAt = now;
            const target = Game.getFormEx(r.worldOrCell);
            if (target) {
                TESModPlatform.moveRefrToPosition(refr, Cell.from(target), WorldSpace.from(target), r.pos[0], r.pos[1], r.pos[2],
                    r.rot[0] * Math.PI / 180, r.rot[1] * Math.PI / 180, r.rot[2] * Math.PI / 180);
            }
            logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: ${hold.asks === 1 ? "asked" : "re-asked"} the move at +${now - hold.since} ms${target ? "" : " (the record's cell is not a form here)"}, the engine at ${engineAt}`);
            return;
        }
        if (hold.asks >= HostService.maxAsks && now - hold.askedAt >= HostService.placementTimeoutMs) {
            this.pendingPlacement.delete(remoteId);
            logLab("verb:npc-handoff", `hold ${remoteId.toString(16)}: not placed after ${now - hold.since} ms (${asked}), the engine at ${engineAt}, the record at ${recordAt}; released on the timeout`);
            this.runDeferred(remoteId, hold.deferred);
        }
    }

    private readonly pendingPlacement = new Map<number, PendingPlacement>();
    private released: Released[] = [];
    private static readonly placedWithinUnits = 64;
    // another ask every 1.5 s while the last did not land, up to five asks
    // (the last at about 6 s, past a cold login's load block of at least 4 s,
    // 5 by LoadGame.cpp's own comment); the release two seconds after the
    // last ask, so a stuck placement holds the actor for about 8 s at most; a
    // copy whose form never appears is released at noFormTimeoutMs. Known
    // limit (review item 6): a death deferred that long carries the engine's
    // position at the hold's end and may miss the server's hit window or its
    // 2048-unit bound, where the server's refusal line says so.
    private static readonly reaskAfterMs = 1500;
    private static readonly maxAsks = 5;
    private static readonly placementTimeoutMs = 2000;
    private static readonly noFormTimeoutMs = 5000;
    // a dead word's hold: the bind kill runs within formView's 2 s check once
    // the 3D loads; past this the server's host stamp holds the death anyway
    private static readonly deadBindTimeoutMs = 8000;
    // a dead hold whose copy never loads 3D is released at this cap
    private static readonly deadHoldCapMs = 120000;

    private onCellFullyLoaded(e: CellFullyLoadedEvent) {
        const id = e.cell.getFormID();
        this.detached.delete(id);
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
                this.detached.set(id, now);
                this.report(id, false);
            }
        }
        while (this.detached.size > HostService.reattachMaxCells) {
            // Map iteration is insertion order: the oldest detach goes first
            const oldest = this.detached.keys().next().value;
            if (oldest === undefined) {
                break;
            }
            this.detached.delete(oldest);
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
            // thuum docs/DECISIONS.md D-L1 B: a login inside a cell (a save
            // load) gets no load event for it, so its entry events are sent
            // once after the grant and the records' apply (LoginCellService)
            this.controller.lookupListener(LoginCellService).noteWithoutLoadEvent(id);
        }
        // a cell the engine re-attaches from its buffer fires no cellFullyLoaded
        // either (thuum b-npc-handoff-door-hosts 20261103-134832: c1's warm exit
        // from the inn re-attached Riverwood's exterior with no event, so only
        // its own cell was re-reported and 9731 stayed unhosted for the run,
        // while a cold exit from lab-spawn fires 27 events; docs/verbs/
        // cell-host.md), so a cell detached within the window is read again
        // each second and reported when it stands attached again
        for (const [id, at] of Array.from(this.detached.entries())) {
            if (now - at > HostService.reattachWindowMs || this.loaded.has(id)) {
                this.detached.delete(id);
                continue;
            }
            if (at === now) {
                // detached in this very poll: read again next second
                continue;
            }
            const cell = this.sp.Cell.from(this.sp.Game.getFormEx(id));
            if (cell !== null && cell.isAttached()) {
                this.detached.delete(id);
                this.loaded.add(id);
                this.report(id, true);
                logLab("verb:cell-host", `cell ${id.toString(16)} re-attached without its load event, reported ${now - at} ms after its detach`);
            }
        }
        if (now - this.lastRefresh >= HostService.refreshMs) {
            this.lastRefresh = now;
            for (const id of Array.from(this.loaded)) {
                this.report(id, true);
            }
        }
        this.persist();
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
        // thuum docs/DECISIONS.md ADR-068 decision 5 and amendment A (rung 3,
        // the script record; docs/research/rung3-script-record-2026-11-06.md
        // 1(d)): the gate (the platform's hosted set, the activation lift)
        // opens once the cell's script records are applied, ScriptStateService's
        // pass on the next update, so this seat's scripts start from the
        // record and its graphs from the record's view before an event or an
        // activation of the cell reaches them; with no record held, the switch
        // scriptRecord off or an older platform, the pass opens it on the next
        // update, as tellPlatform and liftCellActivation did here. The cell's
        // ref state words wait for the same gate (rung 4, RefStateService)
        this.controller.lookupListener(RefStateService).holdForGrant(cell);
        this.controller.lookupListener(ScriptStateService).beforeHosting(cell, () => this.openGate(cell, epoch));
        this.persist();
        logTrace(this, `hosting cell ${cell.toString(16)} at epoch ${epoch}`);
        // thuum playtest 38's finding 66: the roster that raced this grant was
        // dropped as nobody's and the next came seconds later; the server now
        // replays what it held (m3-grant-roster), and this seat reports at
        // its next poll instead of waiting out sightingsMs
        this.lastSightings = 0;
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
        this.tellPlatform(cell, false);
        this.liftCellActivation(cell, false);
        // the cell's known references read once more under the released
        // epoch, the server's release read while the cell is unhosted
        // (ScriptStateService, rung 3)
        this.controller.lookupListener(ScriptStateService).onRelease(cell, epoch);
        if (epoch > (this.lastEpoch.get(cell) ?? 0)) {
            this.lastEpoch.set(cell, epoch);
        }
        this.persist();
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
            this.tellPlatform(cell, false);
            this.liftCellActivation(cell, false);
        }
        this.hosted.clear();
        this.lastEpoch.clear();
        this.persist();
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
    // The state a plugin reload must not lose, as plain arrays in the
    // platform's storage (ADR-041's survivor): a hot-reloaded client (lab
    // client-hot, ADR-054) keeps its connection, so no grant, release or
    // load event comes again. Without it a reloaded host held no cell: the
    // roster stopped (hosted.size 0), isHostedHere said no, a release for
    // a cell it hosts was ignored, and the native hosted mirror
    // (setHosted) could not be cleared. Written at every grant, release
    // and reset and once a second from the poll; read once at construction.
    private persist() {
        this.sp.storage[HOST_STORAGE_KEY] = {
            hosted: Array.from(this.hosted.entries()),
            lastEpoch: Array.from(this.lastEpoch.entries()),
            loaded: Array.from(this.loaded),
            detached: Array.from(this.detached.entries()),
            hostedActors: Array.from(this.hostedActors),
            attached: Array.from(this.attached.keys()),
        };
    }

    private restore() {
        const raw: unknown = this.sp.storage[HOST_STORAGE_KEY];
        if (!raw || typeof raw !== "object") {
            return;
        }
        const stored = raw as Partial<Record<"hosted" | "lastEpoch" | "loaded" | "detached" | "hostedActors" | "attached", unknown>>;
        const pairs = (v: unknown): Array<[number, number]> => Array.isArray(v)
            ? v.filter((x): x is [number, number] => Array.isArray(x) && x.length === 2 && typeof x[0] === "number" && typeof x[1] === "number")
            : [];
        const ids = (v: unknown): number[] => Array.isArray(v) ? v.filter((x): x is number => typeof x === "number") : [];
        for (const [cell, epoch] of pairs(stored.hosted)) {
            this.hosted.set(cell, epoch);
            this.tellPlatform(cell, true);
        }
        for (const [cell, epoch] of pairs(stored.lastEpoch)) {
            this.lastEpoch.set(cell, epoch);
        }
        for (const id of ids(stored.loaded)) {
            this.loaded.add(id);
        }
        for (const [cell, at] of pairs(stored.detached)) {
            this.detached.set(cell, at);
        }
        for (const id of ids(stored.hostedActors)) {
            this.hostedActors.add(id);
        }
        for (const id of ids(stored.attached)) {
            this.attached.set(id, true);
        }
        if (this.hosted.size > 0 || this.loaded.size > 0) {
            logTrace(this, `resumed after a reload: ${this.hosted.size} hosted cell(s), ${this.loaded.size} loaded, ${this.hostedActors.size} streamed actor(s)`);
        }
    }

    private static readonly refreshMs = 30_000;
    // how many detached cells are watched for a silent re-attach: the engine's
    // exterior cell buffer is count-bound (uExterior Cell Buffer, 36 by
    // default; the eviction order HYPOTHESIS), so the newest cells are the
    // ones that can come back without an event; 64 covers an ini above the
    // default. The time window is a backstop for a cell the buffer must have
    // dropped long ago (the fresh review of 31167a9b, note 1)
    private static readonly reattachMaxCells = 64;
    private static readonly reattachWindowMs = 60 * 60_000;
    private static readonly sightingsMs = 2000;
    private static readonly sightingsMax = 64;
    private readonly enabled: boolean;
    private readonly loaded = new Set<number>();
    // the cells that left the loaded set, by the poll's time of the detach
    private readonly detached = new Map<number, number>();
    private readonly hosted = new Map<number, number>();
    // the actors whose actions stream from here (npc-stream), by local id
    private readonly hostedActors = new Set<number>();
    private readonly streamEnabled: boolean;
    private readonly lastEpoch = new Map<number, number>();
    private lastPoll = 0;
    private lastRefresh = 0;
    private lastSightings = 0;
    private readonly attached = new Map<number, true>();
}
