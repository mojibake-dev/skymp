import { Actor } from "skyrimPlatform";
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
// state), the TypeScript of the pipeline over the natives of
// adr065-sys1-native (2ea234cd). The FIRST TRIGGER is ADR-067's corpse (docs/
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
// The natives are TESModPlatform Papyrus statics answering JSON strings,
// called by name through callNative in the update's context (synchronous,
// Skyrim Platform's OnUpdate thread), so no typing of the client's own:
// SerializeChangeForm(refr) answers "" on a refusal (the reason in the
// platform log, "[verb:change-form] SerializeChangeForm refused <id>:
// <reason>") or a JSON object whose keys formType, flags, version, bodyHex
// (the WHOLE record's bytes, lowercase hex), fnv1a (over those bytes) and
// remap ([{offset, formId, index}], offset a RECORD offset with the header's
// width, the convention the wire's validator shares) make the message, the
// rest diagnostics; ApplyChangeForm(refr, bodyHex, flags, version,
// "offset:formId:index,...") answers a JSON object {applied, reason ("" when
// applied, else "at <step>: ..."), fnvPatched, ...diagnostics}. Papyrus Int
// is signed: the u32 flags go in as `flags | 0` and come back `>>> 0`.
//
// The owner's half: sendInputsService keeps sampling a dead hosted actor at
// its cadence until two consecutive samples agree within CORPSE_REST_UNITS
// and CORPSE_REST_DEGREES or CORPSE_SETTLE_MS pass (the server's own rest
// rule), then stops for that body and calls onBodyAtRest here, which
// serializes and sends ONE reliable ChangeForm with the contract's fields
// when its fnv1a differs from the last ChangeFormAck's for the form. The
// epoch is the cell's host epoch this client holds (HostService.
// epochForActor); with none (0) the message is HELD and sent at the next
// grant or the five-second pass, and an unacked message is re-sent with a
// fresh epoch every five seconds up to three tries, then given up (never per
// tick). The watcher's half: ChangeFormApply (served with the actor's
// CreateActor word, at a cell's load and after an owner's record is acked)
// is held until the copy is dead here, then applied once per received record
// (keyed by the body's FNV-1a, computed here: the apply message carries no
// hash). The kill is DEFERRED: Actor.Kill and KillSilent queue a type-10 task
// and the actor is alive at the native's return (docs/research/adr062/
// papyrus-native-entries.md:73; deathService's own 0.1 s wait before the
// push agrees), so deathService's kill marks the record and the apply runs
// as the kill lands, tried every update for killLandsMs with no attempt
// counted while the copy is alive; the bind's record applies on the owner's
// own seat too (a host relaunch must not re-record every corpse in its cells
// at the spawn point). Game-file actors alone in this slice (a server-made
// copy's id is seat-local; its record is read 4's).
//
// HYPOTHESIS until the lab (the acceptance scenario of the verb doc): that
// the owner's reference position follows its ragdoll's root until the rest
// (the owner's rest line carries the distance from the first dead sample, so
// a reference that never moved reads 0 there and rests at its second sample,
// the record then the death point's); that the record applied as the kill
// lands lays the body with no fall (read 2 of the Dynamic plan); that a
// record whose cell is not the copy's moves it (read 3).
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
        h ^= parseInt(hex.slice(i, i + 2), 16);
        h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h >>> 0;
};

// SerializeChangeForm's answer as the message carries it
export type ChangeFormRecord = { formType: number; flags: number; version: number; bodyHex: string; fnv1a: number; remap: ChangeFormRemapRow[] };
// ApplyChangeForm's answer: the three the service reads, the rest of the
// JSON kept as text for the lab line
export type ChangeFormApplyResult = { applied: boolean; reason: string; fnvPatched: number; detail: string };

const asNumber = (v: unknown): number | undefined => (typeof v === "number" && Number.isFinite(v) ? v : undefined);

// the serialize JSON into the record: the u32 flags and hash read back
// unsigned, the census rows' three numbers (a fourth key, formType, is the
// native's diagnostic); undefined for an answer without the record's keys
const parseRecord = (raw: string): ChangeFormRecord | undefined => {
    const o = JSON.parse(raw) as Record<string, unknown> | null;
    if (!o || typeof o !== "object") {
        return undefined;
    }
    const formType = asNumber(o["formType"]);
    const flags = asNumber(o["flags"]);
    const version = asNumber(o["version"]);
    const fnv1a = asNumber(o["fnv1a"]);
    const bodyHex = o["bodyHex"];
    if (formType === undefined || flags === undefined || version === undefined || fnv1a === undefined || typeof bodyHex !== "string") {
        return undefined;
    }
    const rows: ChangeFormRemapRow[] = [];
    const remap = o["remap"];
    if (Array.isArray(remap)) {
        for (const r of remap) {
            const row = (r && typeof r === "object" ? r : {}) as Record<string, unknown>;
            const offset = asNumber(row["offset"]);
            const formId = asNumber(row["formId"]);
            const index = asNumber(row["index"]);
            if (offset !== undefined && formId !== undefined && index !== undefined) {
                rows.push({ offset, formId: formId >>> 0, index });
            }
        }
    }
    return { formType, flags: flags >>> 0, version, bodyHex, fnv1a: fnv1a >>> 0, remap: rows };
};

const parseApplyResult = (raw: string): ChangeFormApplyResult | undefined => {
    const o = JSON.parse(raw) as Record<string, unknown> | null;
    if (!o || typeof o !== "object") {
        return undefined;
    }
    const { applied, reason, fnvPatched, ...rest } = o;
    return { applied: applied === true, reason: typeof reason === "string" ? reason : "", fnvPatched: (asNumber(fnvPatched) ?? 0) >>> 0, detail: JSON.stringify(rest) };
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
    applied: boolean;      // the apply is over: the native took it (laid) or this gave up on it
    laid: boolean;         // the native laid the body (applied true alone can be a give-up)
    attempts: number;
    lastAttemptAt: number;
    heldLogged: boolean;
    killedHereAt: number;  // deathService's kill of the copy, in flight (the task queue) or landed
    bindKill: boolean;     // that kill was the bind's (applies on the owner's own seat too)
};

// the owner's message waiting for an epoch or an ack
type PendingSend = { message: ChangeFormMessage; at: string; heldAt: number; sentAt: number; tries: number };

export class ChangeFormService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("changeFormAckMessage", (e) => this.onAck(e));
        this.controller.emitter.on("changeFormApplyMessage", (e) => this.onApply(e));
        // a grant: a held or unacked record goes out with the fresh epoch on
        // the next update (the grant's handler runs in the message context,
        // where the epoch's reads are refused)
        this.controller.emitter.on("cellHostChanged", (e) => {
            if (e.hosted) {
                this.controller.once("update", () => this.resendPending(Date.now()));
            }
        });
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
        let rec: ChangeFormRecord | undefined;
        try {
            const raw = this.sp.callNative("TESModPlatform", "SerializeChangeForm", undefined, body);
            if (typeof raw !== "string") {
                logLab("verb:npc-fate", `changeform ${hex} not serialized: the native answered no string (${typeof raw})`);
                return;
            }
            if (raw === "") {
                logLab("verb:npc-fate", `changeform ${hex} not serialized: the native refused (the platform log has the reason: "[verb:change-form] SerializeChangeForm refused ${hex}: ...")`);
                return;
            }
            rec = parseRecord(raw);
        } catch (e) {
            logLab("verb:npc-fate", `changeform ${hex} not serialized: the native is missing on this platform or answered no JSON (${e})`);
            return;
        }
        if (!rec) {
            logLab("verb:npc-fate", `changeform ${hex} not serialized: the native's answer has not the record's keys`);
            return;
        }
        const acked = this.acked.get(remoteId);
        if (acked === rec.fnv1a) {
            this.pending.delete(remoteId);
            logTrace(this, `changeform ${hex}: fnv ${rec.fnv1a.toString(16)} is the acked record, nothing sent`);
            return;
        }
        const now = Date.now();
        const at = `at rest (${pos[0].toFixed(0)}, ${pos[1].toFixed(0)}, ${pos[2].toFixed(0)})`;
        const message: ChangeFormMessage = {
            t: MsgType.ChangeForm,
            epoch: 0,
            formId: remoteId,
            formType: rec.formType,
            flags: rec.flags,
            version: rec.version,
            body: rec.bodyHex,
            fnv1a: rec.fnv1a,
            remap: rec.remap,
        };
        // the cell's host epoch this client holds for the actor's cell
        // (ADR-066's stamp; the verb doc's Ownership); none names it: held
        const epoch = this.controller.lookupListener(HostService).epochForActor(body);
        if (epoch === 0) {
            this.pending.set(remoteId, { message, at, heldAt: now, sentAt: 0, tries: 0 });
            logLab("verb:npc-fate", `changeform ${hex} held: no hosted cell names the actor's; sent at the next grant (${rec.bodyHex.length / 2} bytes, fnv ${rec.fnv1a.toString(16)}, ${rec.remap.length} remap rows, ${at})`);
            return;
        }
        this.send({ message, at, heldAt: now, sentAt: 0, tries: 0 }, epoch, now);
    }

    // one send of a pending message with the epoch of the moment; the entry
    // stays until the ack (onAck) or the tries run out (resendPending)
    private send(p: PendingSend, epoch: number, now: number): void {
        const message: ChangeFormMessage = { ...p.message, epoch };
        const tries = p.tries + 1;
        this.pending.set(message.formId, { message, at: p.at, heldAt: p.heldAt, sentAt: now, tries });
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab("verb:npc-fate", `changeform ${message.formId.toString(16)} sent: ${message.body.length / 2} bytes, fnv ${message.fnv1a.toString(16)}, ${message.remap.length} remap rows, ${p.at}; epoch ${epoch}${tries > 1 ? `, try ${tries}` : ""}`);
    }

    // the held and the unacked, on a grant and every resendMs: a fresh epoch
    // and another send, up to maxTries; a message held with no hosted cell
    // for holdMs is dropped. Runs in the update's context
    private resendPending(now: number): void {
        if (this.pending.size === 0) {
            return;
        }
        this.pending.forEach((p, formId) => {
            const hex = formId.toString(16);
            if (p.sentAt !== 0 && now - p.sentAt < ChangeFormService.resendMs) {
                // sent lately: the ack may still come
                return;
            }
            if (p.tries >= ChangeFormService.maxTries) {
                this.pending.delete(formId);
                logLab("verb:npc-fate", `changeform ${hex} unacked after ${p.tries} tries, given up`);
                return;
            }
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(formId));
            const epoch = refr ? this.controller.lookupListener(HostService).epochForActor(refr) : 0;
            if (epoch === 0) {
                if (now - p.heldAt >= ChangeFormService.holdMs) {
                    this.pending.delete(formId);
                    logLab("verb:npc-fate", `changeform ${hex} unsent: no hosted cell named the actor's for ${now - p.heldAt} ms; dropped`);
                }
                return;
            }
            this.send(p, epoch, now);
        });
    }

    // deathService.killActor's NPC branch, as the kill is asked: the kill is
    // deferred (papyrus-native-entries.md:73), so the record is marked and
    // the apply runs as the kill lands (onUpdate's pass). The bind (`bind`,
    // the silent kill of a copy the game files spawned alive while the record
    // is dead) applies on any seat, the owner's own copy included (the record
    // is the previous owner's final state, this seat's truth to start from);
    // a kill by the word on a seat that simulates the actor never takes a
    // held record over its own engine's death (its sender serializes the new
    // one)
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
        rec.killedHereAt = Date.now();
        rec.bindKill = rec.bindKill || bind;
        this.tryApply(rec, "right after the kill here", true);
    }

    // deathService.resurrectActor: the live word stands the body up; a held
    // dead record is stale with it, and so is this seat's unsent one
    onStoodUp(localId: number): void {
        if (this.records.delete(localId)) {
            logTrace(this, `changeform ${localId.toString(16)}: the record dropped, the live word stands the body up`);
        }
        this.pending.delete(localId);
    }

    // a record stands for the form here: held for the kill, or laid on the
    // body (a given-up apply stands for nothing: deathService's push runs as
    // before and the body falls where it is)
    hasRecord(formId: number): boolean {
        const rec = this.records.get(formId);
        return !!rec && (!rec.applied || rec.laid);
    }

    // the native laid the body from the record
    hasAppliedRecord(formId: number): boolean {
        const rec = this.records.get(formId);
        return !!rec && rec.laid;
    }

    private onAck(e: ConnectionMessage<ChangeFormAckMessage>): void {
        const { formId, fnv1a } = e.message;
        this.acked.set(formId, fnv1a);
        this.pending.delete(formId);
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
        if (live && live.fnv === fnv) {
            if (!live.applied) {
                // the same record served twice (with the word and at the
                // cell's load): one apply
                return;
            }
            if (live.laid) {
                // re-served for a body laid here (a cell's load): no re-apply
                // while the copy still reads dead; a copy alive again (a fresh
                // load of the actor) takes it as a new record
                this.controller.once("update", () => {
                    const again = this.records.get(m.formId);
                    if (!again || again.fnv !== fnv || !again.laid) {
                        return;
                    }
                    const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(m.formId));
                    const ac = refr ? this.sp.Actor.from(refr) : null;
                    if (ac && ac.isDead()) {
                        logTrace(this, `changeform ${hex}: the same record re-served for the laid body, no re-apply`);
                        return;
                    }
                    this.hold(m, fnv);
                });
                return;
            }
            // applied but not laid (given up): a fresh serve is a fresh chance
        }
        this.hold(m, fnv);
    }

    // the record held, the apply queued to the next update
    private hold(m: ChangeFormApplyMessage, fnv: number): void {
        if (this.records.size >= ChangeFormService.maxRecords && !this.records.has(m.formId)) {
            // Map iteration is insertion order: the oldest applied record goes
            for (const [id, old] of Array.from(this.records.entries())) {
                if (old.applied) {
                    this.records.delete(id);
                    break;
                }
            }
        }
        this.records.set(m.formId, { formId: m.formId, formType: m.formType, flags: m.flags, version: m.version, body: m.body, remap: m.remap, fnv, receivedAt: Date.now(), applied: false, laid: false, attempts: 0, lastAttemptAt: 0, heldLogged: false, killedHereAt: 0, bindKill: false });
        this.controller.once("update", () => {
            const rec = this.records.get(m.formId);
            if (rec && !rec.applied && rec.fnv === fnv) {
                this.tryApply(rec, "at its arrival", false);
            }
        });
    }

    // the apply, in the update's context: the copy must be dead here (else
    // the record is held for the kill, said once, and said not at all while
    // deathService's kill is in flight); `bind` is the kill's own call or
    // the pass for a bind kill, which applies on the owner's seat too
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
            if (!rec.heldLogged && rec.killedHereAt === 0) {
                rec.heldLogged = true;
                logLab("verb:npc-fate", `corpse ${hex} record held until the kill`);
            }
            return;
        }
        if (!bind && !rec.bindKill && this.controller.lookupListener(HostService).isMineLocal(rec.formId)) {
            // this seat simulates the body and its engine's state stands; its
            // own record goes out from the rest
            this.records.delete(rec.formId);
            logTrace(this, `changeform ${hex}: the record dropped, this seat simulates the body`);
            return;
        }
        const now = Date.now();
        rec.attempts += 1;
        rec.lastAttemptAt = now;
        let result: ChangeFormApplyResult | undefined;
        try {
            const rows = rec.remap.map((r) => `${r.offset}:${r.formId}:${r.index}`).join(",");
            const raw = this.sp.callNative("TESModPlatform", "ApplyChangeForm", undefined, refr, rec.body, rec.flags | 0, rec.version, rows);
            if (typeof raw !== "string" || raw === "") {
                rec.applied = true;
                logLab("verb:npc-fate", `corpse ${hex} not applied: the native answered ${typeof raw === "string" ? "an empty string" : `no string (${typeof raw})`}`);
                return;
            }
            result = parseApplyResult(raw);
        } catch (e) {
            rec.applied = true;
            logLab("verb:npc-fate", `corpse ${hex} not applied: the native is missing on this platform or answered no JSON (${e})`);
            return;
        }
        if (!result) {
            rec.applied = true;
            logLab("verb:npc-fate", `corpse ${hex} not applied: the native's answer is no object`);
            return;
        }
        if (!result.applied) {
            if (rec.attempts >= ChangeFormService.maxAttempts) {
                rec.applied = true;
            }
            logLab("verb:npc-fate", `corpse ${hex} not applied: ${result.reason} (attempt ${rec.attempts} of ${ChangeFormService.maxAttempts}, ${when}); ${result.detail}`);
            return;
        }
        rec.applied = true;
        rec.laid = true;
        logLab("verb:npc-fate", `corpse ${hex} applied from the record: fnv ${rec.fnv.toString(16)}, ${rec.remap.length} rows remapped; patched fnv ${result.fnvPatched.toString(16)}, ${when}, ${now - rec.receivedAt} ms after the record came; ${result.detail}`);
    }

    // every update: the kill in flight (deferred, papyrus-native-entries.md:
    // 73) lands within a frame or two, and the record whose body deathService
    // just killed is tried until it does, for killLandsMs, the first attempt
    // alone (a refusal's retries are the poll's). Every pollMs: a held record
    // is tried again (a kill the hook did not see, a refused apply retried)
    // and dropped after holdMs. Every resendMs: the owner's held and unacked
    // messages
    private onUpdate(): void {
        const now = Date.now();
        if (this.records.size > 0) {
            this.records.forEach((rec) => {
                if (!rec.applied && rec.attempts === 0 && rec.killedHereAt > 0 && now - rec.killedHereAt < ChangeFormService.killLandsMs) {
                    this.tryApply(rec, "as the kill landed", rec.bindKill);
                }
            });
        }
        if (now - this.lastPoll >= ChangeFormService.pollMs) {
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
                    this.tryApply(rec, "at the poll", rec.bindKill);
                }
            });
        }
        if (now - this.lastResend >= ChangeFormService.resendMs) {
            this.lastResend = now;
            this.resendPending(now);
        }
    }

    // the last acked hash per form (the owner's compare baseline)
    private readonly acked = new Map<number, number>();
    // the owner's messages waiting for an epoch or an ack, by form id
    private readonly pending = new Map<number, PendingSend>();
    // the served records by form id (a game-file actor's id is the same on
    // every seat)
    private readonly records = new Map<number, HeldRecord>();
    private lastPoll = 0;
    private lastResend = 0;
    private static readonly pollMs = 2000;
    private static readonly holdMs = 120_000;
    private static readonly maxAttempts = 5;
    private static readonly maxRecords = 128;
    private static readonly killLandsMs = 3000;
    private static readonly resendMs = 5000;
    private static readonly maxTries = 3;
}
