import { ObjectReference } from "skyrimPlatform";
import { logError, logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { ScriptStateApplyMessage } from "../messages/scriptStateApplyMessage";
import { ScriptStateMessage } from "../messages/scriptStateMessage";
import { ScriptHandle, ScriptObjectState, ScriptValue, ScriptVar } from "../messages/scriptStateTypes";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { ObjectAnimationService } from "./objectAnimationService";
import { SpSnippetService } from "./spSnippetService";

// thuum docs/DECISIONS.md ADR-068 decision 5 and amendment A (rung 3, the
// script record; docs/research/rung3-script-record-2026-11-06.md 1(d)): a
// world object's scripts run on its owner (its cell's host) and the record of
// their state travels, so a new host continues the room where the last one
// left it and a late joiner sees it as it stands. R2: the owner computes, the
// server records (wire-rules script_state.rs judges the sender the cell's host
// at the epoch), the receiver applies the owner's bytes.
//
// The owner's half. A reference is due for a read when a call in the seam's
// batch names it as the call's self or as its caller's self (ScriptCallService
// hands every scriptCalls entry over: an owned self's action rows, and its
// state rows as kind "touch"); when a record read here names it through an
// object variable of kind 0 as a non-actor in the same cell (one hop: a
// pillar's myLinkedRef names the lever, whose numPillarsSolved the pillars set
// with no native on the lever); when this seat's grant of its cell is released
// (the cell's known references read once more under the released epoch, which
// the server takes while the cell is unhosted); and every 30 s (the known set
// swept: there is no digest cadence). A due reference is read on the update
// through TESModPlatform.SerializeScriptState at most once a second; a busy
// object (a stack on it, its lock held, not initialized: H-R2) is asked again
// every 250 ms for 10 s, then the last record stands; a read whose FNV-1a is
// the last this seat sent is not sent; the epoch is HostService.epochForActor's.
// There is no ack: a refusal means this seat is not the owner, and after an
// apply the platform's fnvAfter is the baseline.
//
// The receiver's half. A ScriptStateApply is held by form id at its arrival
// (a message handler: no native there but the platform's pending mark, a table
// write the gate reads, set for a record that carries a view, so the gate
// holds the reference's OnLoad and OnCellLoad until the apply; a record with
// no view lets the load handlers run, H-R8). At a grant HostService calls
// beforeHosting(cell, open): on the next update every held record whose
// reference stands in the cell is applied (a new owner always applies), then
// the gate opens (open: the platform's hosted set and the activation lift); a
// record not applied 3 s after the grant is given up and the gate opens
// anyway, said. A record for a cell this seat does not host is a guest's,
// applied when this seat has no seq for the form or one not above the
// record's; a record for a cell this seat already hosts with no grant of it
// pending is dropped. The apply: ApplyScriptState (the platform clears the
// mark and answers the load events the gate held), each held event re-sent
// once through SendScriptEvent, the record's view replayed through
// ObjectAnimationService.playRelayed where this seat's seq for the form is
// another than the record's (a new owner's graph pulled to its script's state,
// a guest's caught up), then the seq raised to the record's. remoteServer
// leaves its lastAnimation replay to a record with a view, and
// ObjectAnimationService's re-sync at a grant (d1f23444) to a pillar whose
// record was not applied there (F6 deletes the re-sync).
//
// Known limits: a persistent exterior reference's parent cell is its
// worldspace's persistent cell (hostService's isMine note), so a grant's pass
// does not take its record (it is applied as a guest's) and the release read
// does not find it; the load events the gate held for a record that is given
// up are not re-sent (the platform names them only in an apply's answer).
//
// The natives are TESModPlatform Papyrus statics called by name through
// callNative in the update's context (changeFormService's shape), answering
// JSON strings; an older platform without them throws: said once, and this
// seat goes on as before (a grant's gate opens on the next update). The
// setting scriptRecord false in skymp5-client-settings.txt turns all of it
// off.
//
// Lines for the lab (verb:scripts; the console tee cuts a line near 128
// bytes, so what a probe reads comes first): "script state <id> sent: <n>
// script(s), <v> variable(s), fnv <x>, epoch <e>[ (the release read)]",
// "script state <id>: the last record stands, busy for <ms> ms (<what>)",
// "script state <id> applied (grant <cell>): stacks <n>, hold <us> us; <n>
// resolved, <n> none, <n> seat-local" ("(guest)" for a guest's) and after it
// "script state <id>: blanked <event> re-sent; view <event> to play (...),
// +<ms> ms", "script records applied at the grant of <cell>: <n> of <n> in
// <ms> ms, the gate open" (once a grant whose records all applied: no other
// line holds "applied at the grant of"), "the gate of <cell> opened after
// <ms> ms: <n> of <m> script record(s) applied, left: ..." (the timeout),
// "script state <id> from the record: dropped, ..." or ": not applied in <ms>
// ms ...", and the 30 s summary "script state in 30 s: ...".
const TAG = "verb:scripts";
const PLAYER_ID = 0x14;
// game-file references end where the server's copies begin (the validator
// refuses a formId at 0xff000000 and up)
const LAST_GAME_FILE_ID = 0xfeffffff;
// the wire's caps (wire-schema skymp.rs, SCRIPT_OBJECTS and SCRIPT_VARS): a
// read past them is not sent, the decoder would refuse it whole
const SCRIPT_OBJECTS = 16;
const SCRIPT_VARS = 512;
// the owner's cadence (the plan's 1(d)): one read a second per reference at
// most, a busy one asked again every 250 ms for 10 s, the known set swept
// every 30 s
const READ_EVERY_MS = 1000;
const BUSY_RETRY_MS = 250;
const BUSY_WINDOW_MS = 10_000;
const SWEEP_MS = 30_000;
// the validator's budget (wire-validate's script_state_budget: 64 at once,
// then 8 a second, UNMEASURED), mirrored as a bucket so a burst of reads waits
// here instead of being refused there (TrapResultService's shape): a refused
// send would stand as sent here and its state never be sent again unchanged
const SEND_BURST = 64;
const SEND_PER_S = 8;
// per update, so a sweep of a big cell spreads over frames (UNMEASURED): the
// due references looked at, and of those the native reads
const CHECKS_PER_UPDATE = 64;
const READS_PER_UPDATE = 8;
// a read's object handles turned into due references, at most (the one hop)
const HOPS_PER_READ = 16;
// a refused read (no attached script, past a cap, an answer that does not fit
// the wire) is not asked again for this long
const REFUSED_BACKOFF_MS = 60_000;
const KNOWN_MAX = 1024;
const DUE_MAX = 1024;
const LAST_SENT_MAX = 4096;
// the receiver: an apply the platform refused (busy, not initialized) is
// tried again every 250 ms; a grant's gate opens GATE_TIMEOUT_MS after the
// grant whatever is left (the plan's 3 s); a guest's record is given up
// GUEST_WINDOW_MS after its arrival, its mark lifted (UNMEASURED: a late
// joiner's cell load)
const APPLY_RETRY_MS = 250;
const GATE_TIMEOUT_MS = 3000;
const GUEST_WINDOW_MS = 10_000;
// a guest's first try waits this long after its record came, so a HostGrant
// that follows its records in a later tick (the server sends a grant's
// records right before it; a big record's fragments can straddle a tick)
// still finds them held and applies them as the new owner's (UNMEASURED)
const GRANT_GRACE_MS = 250;
// a guest's view waits for the reference's 3D, then is played up to
// VIEW_TRIES times a second apart until the graph takes it (remoteServer's
// replay found the graph often not ready at the load); a new owner plays it
// once, before its gate opens, never after (a late play would move the graph
// off a script that has turned since)
const VIEW_WAIT_MS = 250;
const VIEW_RETRY_MS = 1000;
const VIEW_TRIES = 3;
// a per-reference line at most this often
const LINE_EVERY_MS = 10_000;
// the references whose pending mark this plugin set, kept over a plugin
// reload (sp.storage survives one, ADR-041) so a reloaded client lifts them
// instead of leaving their load events held for the session
const MARKS_STORAGE_KEY = "scriptStateMarks";

// a reference due for a read on this seat
type Due = {
    notBefore: number;  // the read at the earliest (the 1 s rule, a busy retry's 250 ms)
    hop: boolean;       // named by another record's object handle: its own handles are not followed
    epoch: number;      // a release's epoch for the cell just released; 0 reads epochForActor at the read
    busySince: number;  // the first busy answer of this round, 0 for none
    busy: string;       // the last busy answer's text
};

// a record served to this seat
type Held = {
    m: ScriptStateApplyMessage;
    scripts: string;    // the record's scripts as ApplyScriptState takes them: the wire's keys, an absent optional for None
    view: string;       // the record's view, its last animation event; "" for none
    receivedAt: number;
    marked: boolean;    // the platform's pending mark is up for it
    claim: number;      // the cell of the grant's pass that applies it, 0 for none (a guest's look decides)
    phase: "apply" | "view";
    nextTryAt: number;
    appliedAt: number;
    reason: string;     // the platform's last refusal, for the give-up line
    viewTries: number;
};

// a grant waiting for its cell's records
type Pass = {
    since: number;
    open: () => void;           // HostService's gate for the grant
    after: Array<() => void>;   // what waits for the gate (ObjectAnimationService's re-sync)
    applied: number;
    taken: Set<number>;
};

type ReadAnswer =
    | { kind: "record"; fnv: number; scripts: ScriptObjectState[]; vars: number }
    | { kind: "busy"; busy: string }
    | { kind: "bad"; why: string };

// ApplyScriptState's answer as this service reads it; a count the platform
// did not answer is undefined and printed "?" (so a missing stacksOnObject is
// never read as H-R4's 0)
type ApplyAnswer = {
    applied: boolean;
    reason: string;
    scripts: number;
    refusedScripts: number;
    resolved: number | undefined;
    none: number | undefined;
    seatLocal: number | undefined;
    stacksOnObject: number | undefined;
    holdUs: number | undefined;
    blanked: string[];
    fnvAfter: number | undefined;
};

const asNumber = (v: unknown): number | undefined => (typeof v === "number" && Number.isFinite(v) ? v : undefined);
const show = (n: number | undefined): string => (n === undefined ? "?" : String(n));

// the platform's JSON into the wire's shapes, the wire's keys alone (the
// read's lab-only names left out, an absent or null optional left absent);
// undefined for a value that does not have them
const pickHandle = (v: unknown): ScriptHandle | undefined => {
    if (!v || typeof v !== "object") {
        return undefined;
    }
    const r = v as Record<string, unknown>;
    const kind = asNumber(r["kind"]);
    const mid = asNumber(r["mid"]);
    const form = asNumber(r["form"]);
    const type = r["type"];
    if (kind === undefined || mid === undefined || form === undefined || typeof type !== "string") {
        return undefined;
    }
    return { kind, mid, form: form >>> 0, type };
};

const pickValue = (v: unknown): ScriptValue | undefined => {
    if (!v || typeof v !== "object") {
        return undefined;
    }
    const r = v as Record<string, unknown>;
    const k = asNumber(r["k"]);
    if (k === undefined) {
        return undefined;
    }
    const out: ScriptValue = { k };
    const i = asNumber(r["i"]);
    if (i !== undefined) {
        out.i = i;
    }
    const f = asNumber(r["f"]);
    if (f !== undefined) {
        out.f = f;
    }
    const b = r["b"];
    if (typeof b === "boolean") {
        out.b = b;
    }
    const s = r["s"];
    if (typeof s === "string") {
        out.s = s;
    }
    if (r["o"] !== undefined && r["o"] !== null) {
        const o = pickHandle(r["o"]);
        if (!o) {
            return undefined;
        }
        out.o = o;
    }
    return out;
};

const pickVar = (v: unknown): ScriptVar | undefined => {
    const out: ScriptVar | undefined = pickValue(v);
    if (!out) {
        return undefined;
    }
    const r = v as Record<string, unknown>;
    const elem = r["elem"];
    if (typeof elem === "string") {
        out.elem = elem;
    }
    const a = r["a"];
    if (a !== undefined && a !== null) {
        if (!Array.isArray(a)) {
            return undefined;
        }
        const values: ScriptValue[] = [];
        for (const e of a) {
            const value = pickValue(e);
            if (!value) {
                return undefined;
            }
            values.push(value);
        }
        out.a = values;
    }
    return out;
};

// the scripts of a read or of a served record; the reason as a string when
// they do not fit the wire (a variable that does not parse breaks its
// positions, so the record goes whole or not at all)
const pickScripts = (v: unknown): ScriptObjectState[] | string => {
    if (!Array.isArray(v)) {
        return "no scripts array";
    }
    if (v.length < 1 || v.length > SCRIPT_OBJECTS) {
        return `${v.length} script(s), the wire takes 1 to ${SCRIPT_OBJECTS}`;
    }
    const out: ScriptObjectState[] = [];
    for (const s of v) {
        const r = (s && typeof s === "object" ? s : {}) as Record<string, unknown>;
        const type = r["type"];
        const state = r["state"];
        const flags = asNumber(r["flags"]);
        const vars = r["vars"];
        if (typeof type !== "string" || type === "" || typeof state !== "string" || flags === undefined || !Array.isArray(vars)) {
            return "a script without its type, state, flags or vars";
        }
        if (vars.length > SCRIPT_VARS) {
            return `${type} has ${vars.length} variables, the wire takes ${SCRIPT_VARS}`;
        }
        const picked: ScriptVar[] = [];
        for (const x of vars) {
            const pv = pickVar(x);
            if (!pv) {
                return `${type}'s variable ${picked.length} does not parse`;
            }
            picked.push(pv);
        }
        out.push({ type, state, flags, vars: picked });
    }
    return out;
};

// SerializeScriptState's answer: {formId, quiescent: true, fnv1a, scripts:
// [{type, state, flags, names, vars}]}, or {quiescent: false, busy: "<what>"}
const parseRead = (raw: string): ReadAnswer => {
    let o: unknown;
    try {
        o = JSON.parse(raw);
    } catch (e) {
        return { kind: "bad", why: `the native answered no JSON (${raw.slice(0, 60)})` };
    }
    if (!o || typeof o !== "object") {
        return { kind: "bad", why: "the native answered no JSON object" };
    }
    const r = o as Record<string, unknown>;
    if (r["quiescent"] === false) {
        const busy = r["busy"];
        return { kind: "busy", busy: typeof busy === "string" && busy !== "" ? busy : "busy" };
    }
    const fnv = asNumber(r["fnv1a"]);
    if (r["quiescent"] !== true || fnv === undefined) {
        return { kind: "bad", why: "the native's answer has no quiescent flag or no fnv1a" };
    }
    const scripts = pickScripts(r["scripts"]);
    if (typeof scripts === "string") {
        return { kind: "bad", why: `the read does not fit the wire: ${scripts}` };
    }
    let vars = 0;
    for (const s of scripts) {
        vars += s.vars.length;
    }
    return { kind: "record", fnv: fnv >>> 0, scripts, vars };
};

// ApplyScriptState's answer: {applied, scripts[], resolved, none, seatLocal,
// stacksOnObject, holdUs, blanked[], fnvAfter}, or {applied: false, reason}
const parseApply = (raw: string): ApplyAnswer | undefined => {
    let o: unknown;
    try {
        o = JSON.parse(raw);
    } catch (e) {
        return undefined;
    }
    if (!o || typeof o !== "object") {
        return undefined;
    }
    const r = o as Record<string, unknown>;
    const scripts: unknown[] = Array.isArray(r["scripts"]) ? r["scripts"] : [];
    const blanked: unknown[] = Array.isArray(r["blanked"]) ? r["blanked"] : [];
    const reason = r["reason"];
    const fnvAfter = asNumber(r["fnvAfter"]);
    return {
        applied: r["applied"] === true,
        reason: typeof reason === "string" ? reason : "",
        scripts: scripts.length,
        refusedScripts: scripts.filter((s) => !!s && typeof s === "object" && (s as Record<string, unknown>)["applied"] === false).length,
        resolved: asNumber(r["resolved"]),
        none: asNumber(r["none"]),
        seatLocal: asNumber(r["seatLocal"]),
        stacksOnObject: asNumber(r["stacksOnObject"]),
        holdUs: asNumber(r["holdUs"]),
        // the gate holds OnLoad and OnCellLoad alone; a longer list is cut
        blanked: blanked.filter((x): x is string => typeof x === "string" && x !== "").slice(0, 8),
        fnvAfter: fnvAfter === undefined ? undefined : fnvAfter >>> 0,
    };
};

// SendScriptEvent's answer, for the line: {sent: true}, or {sent: false,
// reason}; anything else is shown as it came
const describeSend = (raw: unknown): string => {
    if (typeof raw !== "string") {
        return `answered no string (${typeof raw})`;
    }
    try {
        const o = JSON.parse(raw) as Record<string, unknown> | null;
        if (o && typeof o === "object" && typeof o["sent"] === "boolean") {
            const reason = o["reason"];
            return o["sent"] ? "re-sent" : `not re-sent (${typeof reason === "string" ? reason : "refused"})`;
        }
    } catch (e) {
        // the raw answer below
    }
    return `answered ${raw.slice(0, 40)}`;
};

export class ScriptStateService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        // on unless the client setting scriptRecord is false (propService's
        // guarded read: no settings object reads as on)
        const settings = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        this.enabled = !settings || settings["scriptRecord"] !== false;
        if (!this.enabled) {
            logTrace(this, "scriptRecord is off in settings: no script record is read, sent or applied; a grant opens its gate as before");
        }
        this.liftStaleMarks();
        this.controller.emitter.on("scriptStateApplyMessage", (e) => this.onRecord(e));
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // ScriptCallService's, for every call in the scriptCalls batch: its self
    // and its caller's self are due for a read (the batch names selves this
    // seat owns; a caller's self is judged at its read). JS state alone (the
    // batch's context allows no game call)
    noteCall(self: number, callerSelf: number): void {
        if (!this.enabled) {
            return;
        }
        this.noteDue(self, false, 0);
        if (callerSelf !== self) {
            this.noteDue(callerSelf, false, 0);
        }
    }

    // HostService's, at the release of a grant this seat held (a message
    // handler: JS state alone): the cell's known references are read once
    // more under the released epoch, which the server takes while the cell is
    // unhosted and refuses once another seat is granted (StaleEpoch, ADR-066);
    // a pass still waiting for the cell's records is over, its records left
    // to a guest's look
    onRelease(cell: number, epoch: number): void {
        if (!this.enabled) {
            return;
        }
        let due = 0;
        this.known.forEach((knownCell, id) => {
            if (knownCell === cell) {
                this.noteDue(id, false, epoch);
                due++;
            }
        });
        if (due > 0) {
            logTrace(this, `cell ${cell.toString(16)} released at epoch ${epoch}: ${due} known reference(s) due for the release read`);
        }
        const pass = this.passes.get(cell);
        if (pass) {
            this.passes.delete(cell);
            this.held.forEach((h) => {
                if (h.claim === cell) {
                    h.claim = 0;
                }
            });
            logLab(TAG, `the grant of ${cell.toString(16)} was released before its gate opened: ${pass.applied} of ${pass.taken.size} script record(s) applied, the rest left to a guest's look`);
        }
    }

    // HostService's, at a grant (a message handler): the cell's gate waits for
    // the pass on the next update (runPasses), which applies the cell's held
    // records and then calls `open`; with the switch off or ApplyScriptState
    // gone, `open` runs on the next update as the grant's two calls did. A
    // second grant of the cell before the first's gate opened replaces it
    // (the first's `open` would find its epoch gone anyway)
    beforeHosting(cell: number, open: () => void): void {
        if (!this.enabled || this.applyMissing) {
            this.controller.once("update", open);
            return;
        }
        this.grantApplied.forEach((applied, id) => {
            if (applied === cell) {
                this.grantApplied.delete(id);
            }
        });
        const live = this.passes.get(cell);
        this.passes.set(cell, { since: Date.now(), open, after: live ? live.after : [], applied: 0, taken: new Set<number>() });
    }

    // ObjectAnimationService's: `run` after the cell's gate opened (in the
    // same update), or on the next update when no pass waits
    afterHosting(cell: number, run: () => void): void {
        const pass = this.passes.get(cell);
        if (pass) {
            pass.after.push(run);
            return;
        }
        this.controller.once("update", run);
    }

    // a record of the reference was applied by the cell's last grant's pass
    appliedAtGrant(localId: number): boolean {
        return this.grantApplied.has(localId);
    }

    // remoteServer's: whether a record with a view stands for the reference,
    // still being applied ("pending") or applied ("applied", its view played or
    // current here); "none" leaves the record's lastAnimation replay as it was
    viewOf(localId: number): "none" | "pending" | "applied" {
        return this.views.get(localId) ?? "none";
    }

    private noteDue(localId: number, hop: boolean, epoch: number): void {
        if (this.serializeMissing || localId === 0 || localId === PLAYER_ID || localId > LAST_GAME_FILE_ID) {
            return;
        }
        const live = this.due.get(localId);
        if (live) {
            // a direct reason outranks a hop, a release's epoch outranks none
            live.hop = live.hop && hop;
            if (epoch !== 0) {
                live.epoch = epoch;
            }
            return;
        }
        const now = Date.now();
        const refusedUntil = this.refused.get(localId);
        if (refusedUntil !== undefined && now < refusedUntil) {
            return;
        }
        if (this.due.size >= DUE_MAX) {
            this.dueDropped++;
            return;
        }
        const lastRead = this.lastRead.get(localId);
        this.due.set(localId, {
            notBefore: lastRead === undefined ? now : Math.max(now, lastRead + READ_EVERY_MS),
            hop,
            epoch,
            busySince: 0,
            busy: "",
        });
    }

    // a record from the server (a message handler: JS state and the pending
    // mark alone, the apply waits for the update). The newer record replaces
    // a held one. The mark is set for a record with a view that a guest would
    // apply (this seat's seq for the form, SpSnippetService's JS state, not
    // above the record's): one a guest would drop holds no load event, and a
    // new owner applies it in a cell whose load is over
    private onRecord(e: ConnectionMessage<ScriptStateApplyMessage>): void {
        const m = e.message;
        const hex = m.formId.toString(16);
        if (!this.enabled || this.applyMissing) {
            if (!this.recordsIgnoredSaid) {
                this.recordsIgnoredSaid = true;
                logTrace(this, `script records are not applied here (${this.enabled ? "ApplyScriptState is unavailable on this platform" : "scriptRecord is off"}): ${hex}'s and every later one ignored`);
            }
            return;
        }
        if (m.formId === 0 || m.formId > LAST_GAME_FILE_ID) {
            logError(this, `script state ${hex} from the record: dropped, not a game-file reference's id`);
            return;
        }
        const scripts = pickScripts(m.scripts);
        if (typeof scripts === "string") {
            logError(this, `script state ${hex} from the record: dropped, it does not parse (${scripts})`);
            return;
        }
        const view = typeof m.event === "string" ? m.event : "";
        const last = this.controller.lookupListener(SpSnippetService).seqOf(m.formId);
        const mark = view !== "" && (last === undefined || last <= m.seq);
        const old = this.held.get(m.formId);
        let marked = false;
        if (mark) {
            marked = (old !== undefined && old.marked) || this.setPending(m.formId, true);
        } else if (old !== undefined && old.marked) {
            this.setPending(m.formId, false);
        }
        this.held.set(m.formId, {
            m,
            scripts: JSON.stringify(scripts),
            view,
            receivedAt: Date.now(),
            marked,
            claim: old !== undefined ? old.claim : 0,
            phase: "apply",
            nextTryAt: 0,
            appliedAt: 0,
            reason: "",
            viewTries: 0,
        });
        if (view !== "") {
            this.views.set(m.formId, "pending");
        } else {
            this.views.delete(m.formId);
        }
        this.persistMarks();
    }

    // the player's own actor is created: a fresh table on the server's side of
    // this connection (a login, a reconnect, a restart), which serves its
    // records again; HostService clears its grants at the same message
    private onCreateActor(e: ConnectionMessage<CreateActorMessage>): void {
        if (!e.message.isMe) {
            return;
        }
        this.held.forEach((h, id) => {
            if (h.marked) {
                this.setPending(id, false);
            }
        });
        this.held.clear();
        this.views.clear();
        this.passes.clear();
        this.grantApplied.clear();
        this.due.clear();
        this.lastSent.clear();
        this.known.clear();
        this.lastRead.clear();
        this.refused.clear();
        this.persistMarks();
    }

    private onUpdate(): void {
        if (!this.enabled) {
            return;
        }
        const now = Date.now();
        if (this.passes.size > 0) {
            this.runPasses(now);
        }
        if (this.held.size > 0) {
            this.lookHeld(now);
        }
        if (this.due.size > 0 && !this.serializeMissing) {
            this.readDue(now);
        }
        if (now - this.lastSweep >= SWEEP_MS) {
            this.lastSweep = now;
            this.sweep(now);
        }
    }

    // the grants' passes: each takes the held records whose reference stands
    // in its cell and applies them as the new owner's, then opens the gate,
    // or opens it GATE_TIMEOUT_MS after the grant whatever is left
    private runPasses(now: number): void {
        const host = this.controller.lookupListener(HostService);
        for (const [cell, pass] of Array.from(this.passes.entries())) {
            if (this.passes.get(cell) !== pass) {
                continue;
            }
            if (!host.hostsCell(cell)) {
                // the grant is gone without its release (the connection's reset)
                this.passes.delete(cell);
                continue;
            }
            let waiting = 0;
            this.held.forEach((h, id) => {
                if (h.claim !== 0 && h.claim !== cell) {
                    return;
                }
                const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
                if (h.claim === 0) {
                    const parent = refr ? refr.getParentCell() : null;
                    if (!parent || parent.getFormID() !== cell) {
                        return;
                    }
                    h.claim = cell;
                }
                pass.taken.add(id);
                if (!this.step(id, h, refr, now, cell, pass)) {
                    waiting++;
                }
            });
            if (this.passes.get(cell) !== pass) {
                // closed inside a step (ApplyScriptState gone)
                continue;
            }
            if (waiting > 0 && now - pass.since < GATE_TIMEOUT_MS) {
                continue;
            }
            this.closePass(cell, pass, now);
        }
    }

    // the gate opens: what is left of the pass is given up (its marks
    // lifted), HostService's `open` runs, then what waited for the gate
    private closePass(cell: number, pass: Pass, now: number): void {
        const cellHex = cell.toString(16);
        this.passes.delete(cell);
        const left: string[] = [];
        this.held.forEach((h, id) => {
            if (h.claim !== cell) {
                return;
            }
            const hex = id.toString(16);
            left.push(h.phase === "apply" ? `${hex} (${h.reason !== "" ? h.reason : "not tried"})` : `${hex} (view ${h.view}: no 3D)`);
            this.giveUp(id, h, "");
        });
        try {
            pass.open();
        } catch (e) {
            logError(this, `the gate of ${cellHex} threw at its opening`, e);
        }
        const taken = pass.taken.size;
        if (left.length === 0 && pass.applied === taken) {
            if (taken > 0) {
                logLab(TAG, `script records applied at the grant of ${cellHex}: ${pass.applied} of ${taken} in ${now - pass.since} ms, the gate open`);
            }
        } else {
            logLab(TAG, `the gate of ${cellHex} opened after ${now - pass.since} ms: ${pass.applied} of ${taken} script record(s) applied${left.length > 0 ? `, left: ${left.join(", ")}` : ""}`);
        }
        for (const run of pass.after) {
            try {
                run();
            } catch (e) {
                logError(this, `after the gate of ${cellHex}: a hosting step threw`, e);
            }
        }
    }

    // the records no pass takes: a guest's, or one for a cell this seat
    // already hosts (dropped). Who applies is judged at each try, after
    // GRANT_GRACE_MS (a grant may still follow its records)
    private lookHeld(now: number): void {
        const host = this.controller.lookupListener(HostService);
        const snippets = this.controller.lookupListener(SpSnippetService);
        this.held.forEach((h, id) => {
            if (h.claim !== 0) {
                if (this.passes.has(h.claim)) {
                    return;
                }
                h.claim = 0;
            }
            if (now < h.nextTryAt || now - h.receivedAt < GRANT_GRACE_MS) {
                return;
            }
            if (h.phase === "apply" && now - h.receivedAt >= GUEST_WINDOW_MS) {
                this.giveUp(id, h, `not applied in ${now - h.receivedAt} ms (${h.reason !== "" ? h.reason : "no reference here"}); its load events pass from now`);
                return;
            }
            if (h.phase === "view" && now - h.appliedAt >= GUEST_WINDOW_MS) {
                this.giveUp(id, h, `its view ${h.view} not played, no 3D in ${now - h.appliedAt} ms`);
                return;
            }
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
            if (!refr) {
                h.reason = "no reference here";
                h.nextTryAt = now + APPLY_RETRY_MS;
                return;
            }
            const parent = refr.getParentCell();
            const cellId = parent ? parent.getFormID() : 0;
            if (cellId !== 0 && host.hostsCell(cellId)) {
                if (this.passes.has(cellId)) {
                    h.claim = cellId;
                    return;
                }
                if (h.phase === "view") {
                    // a guest's view not played before this seat took the cell:
                    // the graph is this seat's script's now, a late play would
                    // move it off a script that may have turned since
                    logTrace(this, `script state ${id.toString(16)}: the record's view ${h.view} not played, cell ${cellId.toString(16)} is hosted here now`);
                    this.finish(id, h);
                    return;
                }
                this.giveUp(id, h, `dropped, cell ${cellId.toString(16)} is hosted here and no grant of it is pending`);
                return;
            }
            if (h.phase === "apply") {
                if (this.sp.Actor.from(refr)) {
                    this.giveUp(id, h, "dropped, an actor's record (its simulator's state is a later rung)");
                    return;
                }
                const last = snippets.seqOf(id);
                if (last !== undefined && last > h.m.seq) {
                    this.giveUp(id, h, `dropped, this seat's view is newer (seq ${last} above the record's ${h.m.seq})`);
                    return;
                }
            }
            this.step(id, h, refr, now, 0, undefined);
        });
    }

    // one record's next step on this update; true when it is over (applied
    // and its view played or current, or given up). `pass` is the grant's
    // whose cell is `cell`, undefined (cell 0) for a guest's
    private step(id: number, h: Held, refr: ObjectReference | null, now: number, cell: number, pass: Pass | undefined): boolean {
        if (now < h.nextTryAt) {
            return false;
        }
        if (!refr) {
            h.reason = "no reference here";
            h.nextTryAt = now + APPLY_RETRY_MS;
            return false;
        }
        if (h.phase === "apply") {
            const next = this.apply(id, h, refr, now, cell, pass);
            if (next !== "view") {
                return next === "over";
            }
        }
        return this.playView(id, h, refr, now, cell);
    }

    // ApplyScriptState, then the held load events re-sent, the seq raised;
    // "view" when the record's view is still to play here
    private apply(id: number, h: Held, refr: ObjectReference, now: number, cell: number, pass: Pass | undefined): "retry" | "view" | "over" {
        const hex = id.toString(16);
        let raw: unknown;
        try {
            raw = this.sp.callNative("TESModPlatform", "ApplyScriptState", undefined, refr, h.scripts);
        } catch (e) {
            this.applyGone(e);
            return "over";
        }
        const answer = typeof raw === "string" ? parseApply(raw) : undefined;
        if (!answer) {
            this.giveUp(id, h, `not applied, the native answered ${typeof raw === "string" ? `no JSON object (${raw.slice(0, 60)})` : `no string (${typeof raw})`}`);
            return "over";
        }
        if (!answer.applied) {
            h.reason = answer.reason !== "" ? answer.reason : "refused";
            h.nextTryAt = now + APPLY_RETRY_MS;
            return "retry";
        }
        // applied: the platform lifted the pending mark itself
        h.marked = false;
        h.phase = "view";
        h.appliedAt = now;
        this.persistMarks();
        if (answer.fnvAfter !== undefined) {
            this.setLastSent(id, answer.fnvAfter);
        }
        const parent = refr.getParentCell();
        this.rememberKnown(id, parent ? parent.getFormID() : 0);
        const resent = this.resend(refr, answer.blanked);
        const snippets = this.controller.lookupListener(SpSnippetService);
        const last = snippets.seqOf(id);
        const replay = h.view !== "" && last !== h.m.seq;
        snippets.raiseSeq(id, h.m.seq);
        if (pass) {
            pass.applied++;
            this.grantApplied.set(id, cell);
        }
        // two lines, the fields a probe reads first: the console tee cuts a
        // line near 128 bytes (InGameConsolePrinter.cpp, ScriptCallService's
        // census note); H-R4 reads stacks and hold, H-R6 the resolved and
        // seat-local counts
        const here = last === undefined ? "none here" : `${last} here`;
        const viewText = h.view === "" ? "no view" : replay ? `view ${h.view} to play (seq ${h.m.seq}, ${here})` : `view ${h.view} current (seq ${h.m.seq})`;
        logLab(TAG, `script state ${hex} applied (${pass ? `grant ${cell.toString(16)}` : "guest"}): stacks ${show(answer.stacksOnObject)}, hold ${show(answer.holdUs)} us; ${show(answer.resolved)} resolved, ${show(answer.none)} none, ${show(answer.seatLocal)} seat-local`);
        logLab(TAG, `script state ${hex}: ${resent}; ${viewText}${answer.refusedScripts > 0 ? `; ${answer.refusedScripts} of ${answer.scripts} script(s) refused` : ""}, +${now - h.receivedAt} ms`);
        if (!replay) {
            this.finish(id, h);
            return "over";
        }
        h.nextTryAt = now;
        return "view";
    }

    // the record's view through the relay's own play (the side jump first,
    // ObjectAnimationService.playRelayed): once the reference's 3D is here,
    // unless a relay past the record's seq was taken meanwhile (its play is
    // the newer view). A new owner plays once, a guest up to VIEW_TRIES times
    private playView(id: number, h: Held, refr: ObjectReference, now: number, cell: number): boolean {
        const last = this.controller.lookupListener(SpSnippetService).seqOf(id);
        if (last !== undefined && last > h.m.seq) {
            logTrace(this, `script state ${id.toString(16)}: the record's view ${h.view} not played, a relay past its seq ${h.m.seq} came (seq ${last})`);
            this.finish(id, h);
            return true;
        }
        if (!refr.is3DLoaded()) {
            h.nextTryAt = now + VIEW_WAIT_MS;
            return false;
        }
        h.viewTries++;
        const taken = this.controller.lookupListener(ObjectAnimationService).playRelayed(refr, h.view, "the script record");
        if (taken || cell !== 0 || h.viewTries >= VIEW_TRIES) {
            this.finish(id, h);
            return true;
        }
        h.nextTryAt = now + VIEW_RETRY_MS;
        return false;
    }

    // the load events the gate held while the record was pending, each sent
    // once to the reference's scripts (SendScriptEvent, unseenOnly false: the
    // engine raised them and the gate held them), for the apply's line
    private resend(refr: ObjectReference, names: string[]): string {
        if (names.length === 0) {
            return "nothing blanked";
        }
        if (this.sendEventMissing) {
            return `blanked ${names.join(" and ")}, not re-sent (SendScriptEvent unavailable)`;
        }
        const out: string[] = [];
        for (const name of names) {
            try {
                out.push(`${name} ${describeSend(this.sp.callNative("TESModPlatform", "SendScriptEvent", undefined, refr, name, false))}`);
            } catch (e) {
                this.sendEventMissing = true;
                logLab(TAG, `SendScriptEvent unavailable on this platform (${e}); held load events are not re-sent`);
                out.push(`${name} not re-sent`);
                break;
            }
        }
        return `blanked ${out.join(", ")}`;
    }

    private finish(id: number, h: Held): void {
        this.held.delete(id);
        if (h.view !== "") {
            this.views.set(id, "applied");
        }
    }

    // a record given up or dropped: its mark lifted (the reference's load
    // events pass from now; the ones the gate held are not re-sent, the
    // platform names them only in an apply's answer), its view no longer
    // stands for remoteServer's replay; `what` its line ("" for none: the
    // gate's own line names it)
    private giveUp(id: number, h: Held, what: string): void {
        this.held.delete(id);
        this.views.delete(id);
        if (h.marked) {
            h.marked = false;
            this.setPending(id, false);
        }
        this.persistMarks();
        if (what !== "") {
            logLab(TAG, `script state ${id.toString(16)} from the record: ${what}`);
        }
    }

    // ApplyScriptState threw (an older platform): said once, every held
    // record given up, every grant's gate opened now; from here a grant opens
    // its gate on the next update and a record is ignored at its arrival
    private applyGone(e: unknown): void {
        this.applyMissing = true;
        logLab(TAG, `ApplyScriptState unavailable on this platform (${e}); grants open their gates as before`);
        this.held.forEach((h, id) => {
            if (h.marked) {
                this.setPending(id, false);
            }
        });
        this.held.clear();
        this.views.clear();
        this.persistMarks();
        const now = Date.now();
        for (const [cell, pass] of Array.from(this.passes.entries())) {
            this.closePass(cell, pass, now);
        }
    }

    // the due references whose time has come, a bounded number an update
    private readDue(now: number): void {
        let checks = 0;
        let reads = 0;
        for (const [id, d] of Array.from(this.due.entries())) {
            if (checks >= CHECKS_PER_UPDATE || reads >= READS_PER_UPDATE || this.serializeMissing) {
                return;
            }
            if (now < d.notBefore) {
                continue;
            }
            checks++;
            if (this.read(id, d, now)) {
                reads++;
            }
        }
    }

    // one read of a due reference this seat owns; true when the native was
    // asked
    private read(id: number, d: Due, now: number): boolean {
        const hex = id.toString(16);
        // a record held for it is the server's newer word: this seat's state
        // is read after its apply, whose fnvAfter is the baseline
        if (this.held.has(id)) {
            this.due.delete(id);
            return false;
        }
        const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
        // no reference here; an actor (its simulator's state is a later rung);
        // a trap family reference (every seat runs the family, ADR-068 D-S3
        // point 1, and the native refuses it)
        if (!refr || this.sp.Actor.from(refr) || ObjectReferenceEx.isTrapFamily(id)) {
            this.due.delete(id);
            this.known.delete(id);
            return false;
        }
        const parent = refr.getParentCell();
        const cell = parent ? parent.getFormID() : 0;
        if (cell !== 0 && this.passes.has(cell)) {
            // the grant's records first; the calls after the gate opens read it
            this.due.delete(id);
            return false;
        }
        const epoch = d.epoch !== 0 ? d.epoch : this.controller.lookupListener(HostService).epochForActor(refr);
        if (epoch === 0) {
            // no cell this seat hosts names it: not this seat's to record
            this.due.delete(id);
            return false;
        }
        this.refill(now);
        if (this.tokens < 1) {
            // the budget is spent: read again when a send can go
            d.notBefore = now + 1000 / SEND_PER_S;
            return false;
        }
        this.lastRead.set(id, now);
        this.reads++;
        let raw: unknown;
        try {
            raw = this.sp.callNative("TESModPlatform", "SerializeScriptState", undefined, refr);
        } catch (e) {
            this.serializeMissing = true;
            this.due.clear();
            logLab(TAG, `SerializeScriptState unavailable on this platform (${e}); no record is read here`);
            return true;
        }
        if (typeof raw !== "string" || raw === "") {
            // the platform's refusal (no attached script, an actor, the trap
            // family, a non-finite float, a cap, an answer past 160 KiB) is in
            // its log: "[verb:scripts] SerializeScriptState ..."
            this.refuse(id, now);
            if (this.say(id, "refused", now)) {
                logTrace(this, `script state ${hex} not read: the native ${typeof raw === "string" ? "refused (the platform log has the reason)" : `answered no string (${typeof raw})`}; not asked again for ${REFUSED_BACKOFF_MS / 1000} s`);
            }
            return true;
        }
        const answer = parseRead(raw);
        if (answer.kind === "bad") {
            this.refuse(id, now);
            if (this.say(id, "bad", now)) {
                logError(this, `script state ${hex} not read: ${answer.why}`);
            }
            return true;
        }
        if (answer.kind === "busy") {
            this.busyAnswers++;
            if (d.busySince === 0) {
                d.busySince = now;
            }
            d.busy = answer.busy;
            if (now - d.busySince >= BUSY_WINDOW_MS) {
                this.due.delete(id);
                logLab(TAG, `script state ${hex}: the last record stands, busy for ${now - d.busySince} ms (${d.busy})`);
            } else {
                d.notBefore = now + BUSY_RETRY_MS;
            }
            return true;
        }
        this.due.delete(id);
        this.rememberKnown(id, cell);
        if (!d.hop) {
            this.followHops(id, answer.scripts, cell, d.epoch);
        }
        if (answer.fnv === this.lastSent.get(id)) {
            this.unchanged++;
            return true;
        }
        // the form id is the game file's, the same on every seat (read() takes
        // nothing at 0xff000000 and up)
        const message: ScriptStateMessage = { t: MsgType.ScriptState, epoch, formId: id, fnv1a: answer.fnv, scripts: answer.scripts };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        this.tokens -= 1;
        this.setLastSent(id, answer.fnv);
        this.sent++;
        if (d.epoch !== 0 || this.say(id, "sent", now)) {
            logLab(TAG, `script state ${hex} sent: ${answer.scripts.length} script(s), ${answer.vars} variable(s), fnv ${answer.fnv.toString(16)}, epoch ${epoch}${d.epoch !== 0 ? " (the release read)" : ""}${d.hop ? " (named by another record)" : ""}`);
        }
        return true;
    }

    // the one hop: each object variable of kind 0 (a plain form's handle,
    // portable between seats) that names a non-actor reference in the read's
    // own cell is due too, its own handles not followed; a release read's
    // hops carry its epoch
    private followHops(self: number, scripts: ScriptObjectState[], cell: number, epoch: number): void {
        if (cell === 0) {
            return;
        }
        let hops = 0;
        const visit = (o: ScriptHandle | undefined): boolean => {
            if (!o || o.kind !== 0 || o.form === self || o.form === 0 || o.form === PLAYER_ID || o.form > LAST_GAME_FILE_ID || this.due.has(o.form)) {
                return true;
            }
            if (hops >= HOPS_PER_READ) {
                return false;
            }
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(o.form));
            if (!refr || this.sp.Actor.from(refr)) {
                return true;
            }
            const parent = refr.getParentCell();
            if (!parent || parent.getFormID() !== cell) {
                return true;
            }
            hops++;
            this.noteDue(o.form, true, epoch);
            return true;
        };
        for (const s of scripts) {
            for (const v of s.vars) {
                if (!visit(v.o)) {
                    return;
                }
                for (const e of v.a ?? []) {
                    if (!visit(e.o)) {
                        return;
                    }
                }
            }
        }
    }

    // every SWEEP_MS: the known references of the cells this seat hosts are
    // due (an unchanged FNV sends nothing), the tables' tails pruned, and the
    // window's counts said
    private sweep(now: number): void {
        const host = this.controller.lookupListener(HostService);
        this.known.forEach((cell, id) => {
            if (host.hostsCell(cell)) {
                this.noteDue(id, false, 0);
            }
        });
        this.lastRead.forEach((at, id) => {
            if (now - at >= READ_EVERY_MS) {
                this.lastRead.delete(id);
            }
        });
        this.refused.forEach((until, id) => {
            if (now >= until) {
                this.refused.delete(id);
            }
        });
        if (this.reads > 0 || this.busyAnswers > 0 || this.refusals > 0 || this.dueDropped > 0) {
            logLab(TAG, `script state in ${SWEEP_MS / 1000} s: ${this.reads} read, ${this.sent} sent, ${this.unchanged} unchanged, ${this.busyAnswers} busy, ${this.refusals} refused; ${this.known.size} known, ${this.held.size} held${this.dueDropped > 0 ? `; ${this.dueDropped} due dropped past ${DUE_MAX}` : ""}`);
        }
        this.reads = 0;
        this.sent = 0;
        this.unchanged = 0;
        this.busyAnswers = 0;
        this.refusals = 0;
        this.dueDropped = 0;
    }

    private refill(now: number): void {
        this.tokens = Math.min(SEND_BURST, this.tokens + ((now - this.refilledAt) / 1000) * SEND_PER_S);
        this.refilledAt = now;
    }

    private refuse(id: number, now: number): void {
        this.due.delete(id);
        this.known.delete(id);
        this.refused.set(id, now + REFUSED_BACKOFF_MS);
        this.refusals++;
    }

    private rememberKnown(id: number, cell: number): void {
        this.known.delete(id);
        this.known.set(id, cell);
        if (this.known.size > KNOWN_MAX) {
            // Map iteration is insertion order: the longest unread goes
            const oldest = this.known.keys().next().value;
            if (oldest !== undefined) {
                this.known.delete(oldest);
            }
        }
    }

    private setLastSent(id: number, fnv: number): void {
        if (this.lastSent.size >= LAST_SENT_MAX && !this.lastSent.has(id)) {
            // the baselines start over: a resend of an unchanged record is all it costs
            this.lastSent.clear();
        }
        this.lastSent.set(id, fnv);
    }

    // a per-reference line at most once in LINE_EVERY_MS for its kind
    private say(id: number, kind: string, now: number): boolean {
        const key = `${kind}:${id}`;
        const at = this.saidAt.get(key);
        if (at !== undefined && now - at < LINE_EVERY_MS) {
            return false;
        }
        if (this.saidAt.size >= 4096) {
            this.saidAt.clear();
        }
        this.saidAt.set(key, now);
        return true;
    }

    // the platform's pending mark (a Napi export writing the gate's table:
    // safe from any context); false when refused or unavailable (an older
    // platform: said once, no load event waits for a record from then)
    private setPending(id: number, pending: boolean): boolean {
        if (this.pendingMissing) {
            return false;
        }
        try {
            if (this.sp.setScriptRecordPending(id, pending)) {
                return true;
            }
            if (this.say(id, "mark", Date.now())) {
                logError(this, `setScriptRecordPending(${id.toString(16)}, ${pending}) refused`);
            }
        } catch (e) {
            this.pendingMissing = true;
            logLab(TAG, `setScriptRecordPending unavailable on this platform (${e}); no load event waits for a record`);
        }
        return false;
    }

    private persistMarks(): void {
        const ids: number[] = [];
        this.held.forEach((h, id) => {
            if (h.marked) {
                ids.push(id);
            }
        });
        this.sp.storage[MARKS_STORAGE_KEY] = ids;
    }

    // a reloaded plugin (lab client-hot, ADR-054) lifts the marks the last
    // one left set, whose records died with it
    private liftStaleMarks(): void {
        const stored: unknown = this.sp.storage[MARKS_STORAGE_KEY];
        if (Array.isArray(stored)) {
            const ids = stored.filter((x): x is number => typeof x === "number");
            for (const id of ids) {
                this.setPending(id, false);
            }
            if (ids.length > 0) {
                logTrace(this, `resumed after a reload: ${ids.length} script record mark(s) the last plugin left set, lifted`);
            }
        }
        this.sp.storage[MARKS_STORAGE_KEY] = [];
    }

    private readonly enabled: boolean;
    // the owner's: due references, the last read and the last sent FNV per
    // reference, the known set by cell (insertion order, refreshed at each
    // read), the refused ones until when
    private readonly due = new Map<number, Due>();
    private readonly lastRead = new Map<number, number>();
    private readonly lastSent = new Map<number, number>();
    private readonly known = new Map<number, number>();
    private readonly refused = new Map<number, number>();
    // the receiver's: the held records, the views standing for remoteServer,
    // the grants' passes by cell, the references the last pass of their cell
    // applied
    private readonly held = new Map<number, Held>();
    private readonly views = new Map<number, "pending" | "applied">();
    private readonly passes = new Map<number, Pass>();
    private readonly grantApplied = new Map<number, number>();
    private readonly saidAt = new Map<string, number>();
    private lastSweep = Date.now();
    private tokens = SEND_BURST;
    private refilledAt = Date.now();
    private reads = 0;
    private sent = 0;
    private unchanged = 0;
    private busyAnswers = 0;
    private refusals = 0;
    private dueDropped = 0;
    private serializeMissing = false;
    private applyMissing = false;
    private sendEventMissing = false;
    private pendingMissing = false;
    private recordsIgnoredSaid = false;
}
