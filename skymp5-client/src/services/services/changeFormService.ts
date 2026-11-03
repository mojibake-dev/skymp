import { Actor, TESModPlatform } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logLab, logTrace } from "../../logging";
import { NiPoint3 } from "../../sync/movement";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { ConnectionMessage } from "../events/connectionMessage";
import { ChangeFormMessage, ChangeFormRemapRow } from "../messages/changeFormMessage";
import { ChangeFormAckMessage } from "../messages/changeFormAckMessage";
import { ChangeFormApplyMessage } from "../messages/changeFormApplyMessage";
import { HostService } from "./hostService";

// thuum docs/verbs/change-form.md (ADR-065 for system 1: the save is the
// state), the TypeScript of the pipeline, built against the natives of
// adr065-sys1-native by name. The FIRST TRIGGER is ADR-067's corpse (docs/
// DECISIONS.md ADR-067 decision 4, after playtest 42's F4 and F6; read 1's
// GREEN 20261103-210713: c1's dead record of Sven applied on c2's dead copy
// through the engine's own LoadGame moved the copy 1,150 units to c1's place
// in one call and laid it in c1's pose within 5 units): the owner serializes
// its dead hosted actor's own change form when the body is at rest, the
// server records and serves it, and a watcher applies it through its engine
// after its own kill of the copy (the death word's kill, or the bind kill at a
// late load, both kept), so the apply is always dead-on-dead. No SetPosition,
// no SetMotionType, no second kill: the record's life state lays the body and
// its position and cell place the copy (the verb doc's Impose). R2: the owner
// computes, the server records and validates the header, the hash, the
// ownership at the epoch and the remap rows; the watcher renders.
//
// The owner's half: sendInputsService keeps sampling a dead hosted actor at
// its cadence until two consecutive samples agree within CORPSE_REST_UNITS
// and CORPSE_REST_DEGREES or CORPSE_SETTLE_MS pass (the server's own rest
// rule), then stops for that body and calls onBodyAtRest here:
// TESModPlatform.serializeChangeForm(refr) (the on-demand recipe, the
// engine's own SaveGame into a buffer plus the id census) and ONE reliable
// ChangeForm with the contract's fields, sent when its fnv1a differs from the
// last ChangeFormAck's for the form. The watcher's half: ChangeFormApply
// (served with the actor's CreateActor word and at a cell's load, and after
// an owner's record is acked) is held until the copy is dead here, then
// TESModPlatform.applyChangeForm(refr, bodyHex, flags, version, remapRows)
// runs once per received record (keyed by the body's FNV-1a, computed here:
// the apply message carries no hash). deathService's kill is the hook for a
// held record; a record arriving on a dead copy is applied at its arrival.
// Game-file actors alone in this slice (a server-made copy's id is
// seat-local; its record is read 4's).
//
// HYPOTHESIS until the lab (the acceptance scenario of the verb doc): that
// the owner's reference position follows its ragdoll's root until the rest
// (the owner's rest line carries the distance from the first dead sample, so
// a reference that never moved reads 0 there and rests at its second sample,
// the record then the death point's); that the record applied right after
// the kill on this seat lays the body with no fall (read 2 of the Dynamic
// plan: a dead record on a live copy is the bind's shape, applied here after
// the silent kill); that a record whose cell is not the copy's moves it
// (read 3).
export const CORPSE_REST_UNITS = 2;
export const CORPSE_REST_DEGREES = 2;
export const CORPSE_SETTLE_MS = 15_000;

// the shortest signed turn from heading b to heading a, in (-180, 180]
// (movementApply.ts's angleDelta)
const angleDelta = (a: number, b: number): number => ((((a - b + 180) % 360) + 360) % 360) - 180;

// two samples one rest: within CORPSE_REST_UNITS and, on every axis, within
// CORPSE_REST_DEGREES
export const corpseSamplesAgree = (a: { pos: NiPoint3; rot: NiPoint3 }, b: { pos: NiPoint3; rot: NiPoint3 }): boolean => {
    if (ObjectReferenceEx.getDistance(a.pos, b.pos) > CORPSE_REST_UNITS) {
        return false;
    }
    for (let i = 0; i < 3; ++i) {
        if (Math.abs(angleDelta(a.rot[i], b.rot[i])) > CORPSE_REST_DEGREES) {
            return false;
        }
    }
    return true;
};

// FNV-1a, 32 bits, over the bytes a hex string spells (two characters per
// byte): the owner's fnv1a over the same bytes, the receiver's key for a
// record
export const fnv1a32Hex = (hex: string): number => {
    let h = 0x811c9dc5;
    for (let i = 0; i + 1 < hex.length; i += 2) {
        h ^= parseInt(hex.substr(i, 2), 16);
        h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h >>> 0;
};

// a served record on this seat: the message's fields, its hash, and the
// apply's state
type HeldRecord = {
    formId: number;
    formType: number;
    flags: number;
    version: number;
    body: string;
    remap: ChangeFormRemapRow[];
    fnv: number;
    receivedAt: number;
    applied: boolean;      // the native took it (or gave up on it)
    attempts: number;
    lastAttemptAt: number;
    heldLogged: boolean;
};

export class ChangeFormService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("changeFormAckMessage", (e) => this.onAck(e));
        this.controller.emitter.on("changeFormApplyMessage", (e) => this.onApply(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // the owner's trigger (sendInputsService, the at-rest moment of a dead
    // hosted actor): the body's own change form, serialized by the engine's
    // recipe and sent when it differs from the acked record. Runs in the
    // update's context (the sender's tick), where the native is allowed
    onBodyAtRest(remoteId: number, body: Actor, pos: NiPoint3): void {
        const hex = remoteId.toString(16);
        if (remoteId >= 0xff000000) {
            logTrace(this, `changeform ${hex}: a server-made copy's record is not this slice's (game-file actors alone)`);
            return;
        }
        // a platform without the native (an older dist) throws: one line, the
        // sender's tick goes on (hostService.tellPlatform's shape)
        let rec: ReturnType<typeof TESModPlatform.serializeChangeForm>;
        try {
            rec = TESModPlatform.serializeChangeForm(body);
        } catch (e) {
            logLab("verb:npc-fate", `changeform ${hex} not serialized: the native is missing on this platform (${e})`);
            return;
        }
        if (!rec) {
            logLab("verb:npc-fate", `changeform ${hex} not serialized: the native answered null`);
            return;
        }
        const acked = this.acked.get(remoteId);
        if (acked === rec.fnv1a) {
            logTrace(this, `changeform ${hex}: fnv ${rec.fnv1a.toString(16)} is the acked record, nothing sent`);
            return;
        }
        // the cell's host epoch this client holds for the actor's cell
        // (ADR-066's stamp; the verb doc's Ownership)
        const epoch = this.controller.lookupListener(HostService).epochForActor(body);
        const message: ChangeFormMessage = {
            t: MsgType.ChangeForm,
            epoch,
            formId: remoteId,
            formType: rec.formType,
            flags: rec.flags,
            version: rec.version,
            body: rec.bodyHex,
            fnv1a: rec.fnv1a,
            remap: rec.remap,
        };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab("verb:npc-fate", `changeform ${hex} sent: ${rec.bodyHex.length / 2} bytes, fnv ${rec.fnv1a.toString(16)}, ${rec.remap.length} remap rows, at rest (${pos[0].toFixed(0)}, ${pos[1].toFixed(0)}, ${pos[2].toFixed(0)}); epoch ${epoch}${epoch === 0 ? " (no hosted cell names the actor's; the server refuses E_CHANGEFORM_EPOCH)" : ""}`);
    }

    // deathService.killActor's NPC branch, right after the kill: a record
    // held for this body is applied now, the body dead. The bind (`bind`, the
    // silent kill of a copy the game files spawned alive while the record is
    // dead) applies on any seat, the owner's own copy included (the record is
    // the previous owner's final state, this seat's truth to start from); a
    // kill by the word on a seat that simulates the actor never takes a held
    // record over its own engine's death (its sender serializes the new one)
    onKilledHere(localId: number, bind: boolean): void {
        const rec = this.records.get(localId);
        if (!rec || rec.applied) {
            return;
        }
        if (!bind && this.controller.lookupListener(HostService).isMineLocal(localId)) {
            this.records.delete(localId);
            logTrace(this, `changeform ${localId.toString(16)}: the held record dropped, this seat's own engine killed the body`);
            return;
        }
        this.tryApply(rec, "right after the kill here", true);
    }

    // deathService.resurrectActor: the live word stands the body up; a held
    // dead record is stale with it
    onStoodUp(localId: number): void {
        if (this.records.delete(localId)) {
            logTrace(this, `changeform ${localId.toString(16)}: the record dropped, the live word stands the body up`);
        }
    }

    // a record stands for the form here, held for the kill or applied
    // (formView's bind: no move by coordinates, the record lays the body)
    hasRecord(formId: number): boolean {
        return this.records.has(formId);
    }

    // the record was applied by the engine here (deathService's push after a
    // silent kill is skipped: it would undo the laid pose)
    hasAppliedRecord(formId: number): boolean {
        const rec = this.records.get(formId);
        return !!rec && rec.applied && rec.attempts > 0;
    }

    private onAck(e: ConnectionMessage<ChangeFormAckMessage>): void {
        const { formId, fnv1a } = e.message;
        this.acked.set(formId, fnv1a);
        logTrace(this, `changeform ${formId.toString(16)}: acked fnv ${fnv1a.toString(16)}`);
    }

    // a message handler: no native here; the record is held and its apply
    // queued to the next update
    private onApply(e: ConnectionMessage<ChangeFormApplyMessage>): void {
        const m = e.message;
        const hex = m.formId.toString(16);
        if (m.formId >= 0xff000000) {
            logTrace(this, `changeform ${hex}: a server-made copy's record is not this slice's, dropped`);
            return;
        }
        const fnv = fnv1a32Hex(m.body);
        const live = this.records.get(m.formId);
        if (live && !live.applied && live.fnv === fnv) {
            // the same record served twice (with the word and at the cell's
            // load): one apply
            return;
        }
        if (this.records.size >= ChangeFormService.maxRecords && !this.records.has(m.formId)) {
            // Map iteration is insertion order: the oldest applied record goes
            for (const [id, old] of Array.from(this.records.entries())) {
                if (old.applied) {
                    this.records.delete(id);
                    break;
                }
            }
        }
        this.records.set(m.formId, { formId: m.formId, formType: m.formType, flags: m.flags, version: m.version, body: m.body, remap: m.remap, fnv, receivedAt: Date.now(), applied: false, attempts: 0, lastAttemptAt: 0, heldLogged: false });
        this.controller.once("update", () => {
            const rec = this.records.get(m.formId);
            if (rec && !rec.applied && rec.fnv === fnv) {
                this.tryApply(rec, "at its arrival", false);
            }
        });
    }

    // the apply, in the update's context: the copy must be dead here (else
    // the record is held for the kill, said once); `bind` is the kill's own
    // call, which applies on the owner's seat too
    private tryApply(rec: HeldRecord, when: string, bind: boolean): void {
        const hex = rec.formId.toString(16);
        const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(rec.formId));
        const ac = refr ? this.sp.Actor.from(refr) : null;
        if (!refr || !ac) {
            // a game-file actor's form is always here; nothing else is this
            // slice's
            logTrace(this, `changeform ${hex}: no actor form here, the record waits`);
            return;
        }
        if (!ac.isDead()) {
            if (!rec.heldLogged) {
                rec.heldLogged = true;
                logLab("verb:npc-fate", `corpse ${hex} record held until the kill`);
            }
            return;
        }
        if (!bind && this.controller.lookupListener(HostService).isMineLocal(rec.formId)) {
            // this seat simulates the body and its engine's state stands; its
            // own record goes out from the rest
            this.records.delete(rec.formId);
            logTrace(this, `changeform ${hex}: the record dropped, this seat simulates the body`);
            return;
        }
        const now = Date.now();
        rec.attempts += 1;
        rec.lastAttemptAt = now;
        let result: ReturnType<typeof TESModPlatform.applyChangeForm>;
        try {
            result = TESModPlatform.applyChangeForm(refr, rec.body, rec.flags, rec.version, rec.remap);
        } catch (e) {
            rec.applied = true;
            logLab("verb:npc-fate", `corpse ${hex} not applied: the native is missing on this platform (${e})`);
            return;
        }
        if (!result) {
            rec.applied = true;
            logLab("verb:npc-fate", `corpse ${hex} not applied: the native answered null (an older platform)`);
            return;
        }
        if (!result.applied) {
            if (rec.attempts >= ChangeFormService.maxAttempts) {
                rec.applied = true;
            }
            logLab("verb:npc-fate", `corpse ${hex} not applied: ${result.reason} (attempt ${rec.attempts} of ${ChangeFormService.maxAttempts}, ${when})`);
            return;
        }
        rec.applied = true;
        logLab("verb:npc-fate", `corpse ${hex} applied from the record: fnv ${rec.fnv.toString(16)}, ${rec.remap.length} rows remapped; patched fnv ${result.fnvPatched.toString(16)}, ${when}, ${now - rec.receivedAt} ms after the record came`);
    }

    // the poll: a held record is tried again every pollMs (a kill the hook
    // did not see, a refused apply retried) and dropped after holdMs
    private onUpdate(): void {
        if (this.records.size === 0) {
            return;
        }
        const now = Date.now();
        if (now - this.lastPoll < ChangeFormService.pollMs) {
            return;
        }
        this.lastPoll = now;
        this.records.forEach((rec, formId) => {
            if (rec.applied) {
                return;
            }
            if (now - rec.receivedAt >= ChangeFormService.holdMs) {
                this.records.delete(formId);
                logLab("verb:npc-fate", `corpse ${formId.toString(16)} record dropped: held ${now - rec.receivedAt} ms, the copy never died here`);
                return;
            }
            if (rec.attempts === 0 || now - rec.lastAttemptAt >= ChangeFormService.pollMs) {
                this.tryApply(rec, "at the poll", false);
            }
        });
    }

    // the last acked hash per form (the owner's compare baseline)
    private readonly acked = new Map<number, number>();
    // the served records by form id (a game-file actor's id is the same on
    // every seat)
    private readonly records = new Map<number, HeldRecord>();
    private lastPoll = 0;
    private static readonly pollMs = 2000;
    private static readonly holdMs = 120_000;
    private static readonly maxAttempts = 5;
    private static readonly maxRecords = 128;
}
