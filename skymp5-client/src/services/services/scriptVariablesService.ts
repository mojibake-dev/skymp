import { MsgType } from "../../messages";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { ScriptVariableEntry, ScriptVariableKey, ScriptVariablesMessage } from "../messages/scriptVariablesMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// the verb's head (the condition variables are quest-stage.md's rung 5)
const TAG = "verb:quest-stage";
// How often the watched keys are read, ms (D-V1 B: on every seat)
const READ_MS = 1000;
// the validator's budget (wire-validate's script_variables_budget: 4 at
// once, then 1 a second, UNMEASURED), mirrored as a bucket so a read's
// changes past one message wait here instead of being refused there
const REPORT_BURST = 4;
const REPORT_PER_S = 1;
// the wire's cap on one message's keys and entries (cap::SCRIPT_VARIABLES)
const SCRIPT_VARIABLES = 256;
// the server's values written per update (the plan's (d))
const APPLIES_PER_UPDATE = 32;
// the server's record holds at most this many keys (VARIABLE_RECORD_CAP)
const WATCH_MAX = 1024;
const SUMMARY_MS = 30_000;
// load-order forms end where the server's copies begin (the validator
// refuses a form of 0 or at 0xff000000 and up)
const LAST_GAME_FILE_ID = 0xfeffffff;
// rung 3's folded kinds, the ones a condition compares: int, float, bool
const KIND_INT = 3;
const KIND_FLOAT = 4;
const KIND_BOOL = 5;
// D-T2 B: the own entry that carries a form's game-time registration, which
// a relaunch loses (the platform's registration read answers it from
// SkyrimVM's queuedOnUpdateGameEvents, CommonLibSSE-NG SkyrimVM.h:143-158,
// :246-249): f the repeating interval in game hours, 0 for none. Never
// written as a variable; re-issued through Form.RegisterForUpdateGameTime.
// Compared without case, as Papyrus names are
const UPDATE_GAME_TIME_VAR = "::updategametime";
// The storage key of the watch and the epoch (sp.storage survives a plugin
// reload, ADR-041's survivor): the serve comes once, at the login, and a
// hot-reloaded client keeps its connection and never logs in again
const WATCH_STORAGE_KEY = "thuum.scriptVariables.watch.v1";

type Value = number | boolean;

// thuum docs/DECISIONS.md ADR-068 rung 5 (D-V1 B, D-T2 B; the plan's 1(d),
// docs/research/rung5-quests-2026-11-09.md; worldGlobalService.ts's shape):
// the variables dialogue conditions read follow the seat whose engine wrote
// them. A condition-read variable (CTDA 629 GetVMQuestVariable, 630
// GetVMScriptVariable; CommonLibSSE-NG TESCondition.h:762-763) is nobody's
// (D-F1 C), and most are written by a topic-info fragment that runs on the
// talking seat alone (458 of 770 keys, the plan's section 7: H-CV1 fails),
// so every seat reports a listed variable its engine changed, the latest
// write winning per key with the server's sequence (AliasFill 79's shape).
// R2: the writing seat's engine computes, the server records.
//
// The watch. The login's serve (served true) names the keys to read, a
// quest's or a reference's attached script variables by (form, script,
// var); a key the server serves as this player's own is watched too.
// TESModPlatform.ReadScriptVariables(keysJson) reads them once a second, by
// name under the platform's attachedScriptsLock, and answers each key's
// [{form, script, var, k, i|f|b}] or a refusal. A key's first read here is
// its baseline, never a report, so the save's or the template's value is
// never sent over the world's; after it, a change goes out as
// ScriptVariables {epoch, watch [], entries, served false} through the
// validator's bucket, at most 256 entries a message. A key whose served or
// relayed value is still to be written is not compared, and a value written
// here from the server is the key's baseline, so nothing applied is
// reported back. The epoch is the last one the server named on this
// connection (1 before any: the validator refuses 0 from a client).
//
// The receivers. A relay and the login's serve are held by key, the newest
// winning, and written through TESModPlatform.WriteScriptVariables
// (Variable's setters, refusing a kind other than the declared),
// APPLIES_PER_UPDATE at most an update. D-T2 B: the serve's `own` entries,
// this player's own quests' state (the marked quests' scalar variables:
// PlayerVampireQuest's VampireStatus, LastFeedTime, FeedTimer), are written
// first, before the watch's first read here (the gate: no report of this
// seat's fresh values over its own record), and an own entry on
// UPDATE_GAME_TIME_VAR re-issues that form's game-time registration
// (Form.RegisterForUpdateGameTime, every f game hours) instead of a write.
// Rule 5: a form must be a load-order id, the names non-empty, the kind 3 to
// 5 with its payload, a float finite.
//
// callNative needs the Papyrus VM's context (Skyrim Platform refuses natives
// in a message handler): a message is kept and read on the next update. An
// older platform without the natives throws there: said once, and this seat
// reads or writes nothing. The client setting scriptVariables false turns
// the service off.
//
// Lines for the lab (verb:quest-stage): "own state: <n> served value(s)
// written for this player, <m> game-time registration(s) re-issued" once a
// serve, and the 30 s summary "script variables in 30 s: <r> read, <c>
// changed, <s> sent, <a> written (<o> own), <h> here already, <x> refused".
export class ScriptVariablesService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("scriptVariablesMessage", (e) => this.onMessage(e));
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.controller.on("update", () => this.onUpdate());
        this.resume();
    }

    // a relay or the login's serve (a message handler: JS state alone,
    // written on the update)
    private onMessage(e: ConnectionMessage<ScriptVariablesMessage>) {
        const m = e.message;
        if (typeof m.epoch === "number" && Number.isInteger(m.epoch) && m.epoch > 0) {
            this.epoch = m.epoch;
        }
        const watch = Array.isArray(m.watch) ? m.watch : [];
        let added = 0;
        for (const key of watch) {
            if (!ScriptVariablesService.isKey(key)) {
                this.refused++;
                continue;
            }
            if (this.addWatch(key)) {
                added++;
            }
        }
        const own = Array.isArray(m.own) ? m.own : [];
        for (const entry of own) {
            if (!ScriptVariablesService.isEntry(entry)) {
                this.refused++;
                continue;
            }
            const id = ScriptVariablesService.keyId(entry);
            this.toApply.delete(id);
            this.toApplyOwn.delete(id);
            this.toApplyOwn.set(id, entry);
            if (this.addWatch(entry)) {
                added++;
            }
        }
        if (own.length > 0) {
            this.ownServed = true;
        }
        const entries = Array.isArray(m.entries) ? m.entries : [];
        for (const entry of entries) {
            if (!ScriptVariablesService.isEntry(entry)) {
                this.refused++;
                continue;
            }
            const id = ScriptVariablesService.keyId(entry);
            if (this.toApplyOwn.has(id)) {
                continue; // this player's own value wins at the login
            }
            this.toApply.delete(id);
            this.toApply.set(id, entry);
        }
        if (added > 0) {
            this.keysText = undefined;
            this.persist();
            if (m.served) {
                logTrace(this, `watching ${this.watch.size} script variable(s), ${added} new from the serve`);
            }
        }
    }

    // a new connection (connectionAccepted, ahead of all its messages): the
    // server serves the watch and the record anew
    private onConnectionAccepted() {
        this.watch.clear();
        this.last.clear();
        this.toApply.clear();
        this.toApplyOwn.clear();
        this.outgoing.clear();
        this.keysText = undefined;
        this.epoch = 0;
        this.persist();
    }

    private onUpdate() {
        if (!this.enabled()) {
            if (!this.offSaid) {
                this.offSaid = true;
                logTrace(this, "scriptVariables is off in settings: no script variable is read, reported or written here");
            }
            return;
        }
        const now = Date.now();
        if (!this.writeMissing && (this.toApplyOwn.size > 0 || this.toApply.size > 0)) {
            this.applySome();
        }
        // the own state first (the gate): no read before it is written
        if (this.watch.size > 0 && this.toApplyOwn.size === 0 && !this.readMissing && now - this.lastRead >= READ_MS) {
            this.lastRead = now;
            this.collect(this.read());
        }
        if (this.outgoing.size > 0) {
            this.send(now);
        }
        this.summarize(now);
    }

    // the server's values, a bounded number an update, this player's own
    // first; a value equal to the key's last here is not written again
    private applySome() {
        const batch: ScriptVariableEntry[] = [];
        let fromOwn = 0;
        let registrations = 0;
        for (const [id, entry] of Array.from(this.toApplyOwn.entries())) {
            if (batch.length >= APPLIES_PER_UPDATE) {
                break;
            }
            this.toApplyOwn.delete(id);
            if (entry.var.toLowerCase() === UPDATE_GAME_TIME_VAR) {
                if (this.reissue(entry)) {
                    registrations++;
                }
                continue;
            }
            if (this.isHere(id, entry)) {
                this.hereAlready++;
                continue;
            }
            batch.push(entry);
            fromOwn++;
        }
        for (const [id, entry] of Array.from(this.toApply.entries())) {
            if (batch.length >= APPLIES_PER_UPDATE) {
                break;
            }
            this.toApply.delete(id);
            if (this.isHere(id, entry)) {
                this.hereAlready++;
                continue;
            }
            batch.push(entry);
        }
        if (batch.length > 0) {
            this.write(batch, fromOwn);
        }
        this.ownRegistrations += registrations;
        if (this.ownServed && this.toApplyOwn.size === 0) {
            this.ownServed = false;
            logLab(TAG, `own state: ${this.ownWritten} served value(s) written for this player, ${this.ownRegistrations} game-time registration(s) re-issued`);
            this.ownWritten = 0;
            this.ownRegistrations = 0;
        }
    }

    // WriteScriptVariables for a batch; each entry the native did not refuse
    // becomes its key's last value here, so the next read does not report it
    // back. The answer: an array, one element an entry, true or an object
    // without `error` for a write and false, a string or an object with
    // `error` for a refusal; any other answer counts as written and unusable
    private write(batch: ScriptVariableEntry[], fromOwn: number) {
        let raw: unknown;
        try {
            raw = this.sp.callNative("TESModPlatform", "WriteScriptVariables", undefined, JSON.stringify(batch.map(ScriptVariablesService.wireEntry)));
        } catch (e) {
            this.writeMissing = true;
            this.toApply.clear();
            this.toApplyOwn.clear();
            logLab(TAG, `WriteScriptVariables unavailable on this platform (${e}); no script variable is written here`);
            return;
        }
        let answers: unknown[] | undefined;
        if (typeof raw === "string") {
            try {
                const parsed: unknown = JSON.parse(raw);
                answers = Array.isArray(parsed) && parsed.length === batch.length ? parsed : undefined;
            } catch (e) {
                answers = undefined;
            }
        }
        if (!answers) {
            this.badAnswers++;
        }
        batch.forEach((entry, n) => {
            const answer = answers ? answers[n] : true;
            if (ScriptVariablesService.isRefusal(answer)) {
                this.refused++;
                return;
            }
            this.last.set(ScriptVariablesService.keyId(entry), ScriptVariablesService.valueOf(entry));
            this.applied++;
            if (n < fromOwn) {
                this.appliedOwn++;
                this.ownWritten++;
            }
        });
    }

    // D-T2 B: a served game-time registration, issued again on its form (a
    // relaunch's fresh game has none); 0 is no registration
    private reissue(entry: ScriptVariableEntry): boolean {
        const hours = entry.k === KIND_FLOAT ? entry.f : undefined;
        this.last.set(ScriptVariablesService.keyId(entry), typeof hours === "number" ? hours : 0);
        if (typeof hours !== "number" || !Number.isFinite(hours) || hours <= 0) {
            return false;
        }
        const form = this.sp.Form.from(this.sp.Game.getFormEx(entry.form));
        if (!form) {
            this.refused++;
            logError(this, `the served game-time registration of ${entry.form.toString(16)} names no form here`);
            return false;
        }
        form.registerForUpdateGameTime(hours);
        logTrace(this, `the game-time registration of ${entry.form.toString(16)} re-issued: every ${hours} game hour(s)`);
        return true;
    }

    // ReadScriptVariables over the watch; undefined when the native is gone
    // or answered nothing usable
    private read(): ScriptVariableEntry[] | undefined {
        if (this.keysText === undefined) {
            this.keysText = JSON.stringify(Array.from(this.watch.values()).map((k) => ({ form: k.form, script: k.script, var: k.var })));
        }
        let raw: unknown;
        try {
            raw = this.sp.callNative("TESModPlatform", "ReadScriptVariables", undefined, this.keysText);
        } catch (e) {
            this.readMissing = true;
            logLab(TAG, `ReadScriptVariables unavailable on this platform (${e}); this seat reports no script variable`);
            return undefined;
        }
        if (typeof raw !== "string") {
            this.badAnswers++;
            return undefined;
        }
        let parsed: unknown;
        try {
            parsed = JSON.parse(raw);
        } catch (e) {
            this.badAnswers++;
            return undefined;
        }
        if (!Array.isArray(parsed)) {
            this.badAnswers++;
            return undefined;
        }
        this.reads++;
        const out: ScriptVariableEntry[] = [];
        for (const item of parsed) {
            if (ScriptVariablesService.isEntry(item)) {
                out.push(item);
            } else {
                this.readRefused++; // a key the platform refused (its log has the reason)
            }
        }
        return out;
    }

    // a read's values against each key's last here: a key's first read is its
    // baseline; a change goes out, the latest per key; a key with a server
    // value still to write is not compared
    private collect(values: ScriptVariableEntry[] | undefined) {
        if (!values) {
            return;
        }
        for (const entry of values) {
            const id = ScriptVariablesService.keyId(entry);
            if (!this.watch.has(id) || this.toApply.has(id) || this.toApplyOwn.has(id)) {
                continue;
            }
            const value = ScriptVariablesService.valueOf(entry);
            const prev = this.last.get(id);
            this.last.set(id, value);
            if (prev === undefined || ScriptVariablesService.same(prev, value)) {
                continue;
            }
            this.changed++;
            this.outgoing.delete(id);
            this.outgoing.set(id, ScriptVariablesService.wireEntry(entry));
        }
    }

    private send(now: number) {
        this.tokens = Math.min(REPORT_BURST, this.tokens + ((now - this.refilledAt) / 1000) * REPORT_PER_S);
        this.refilledAt = now;
        while (this.outgoing.size > 0 && this.tokens >= 1) {
            const entries: ScriptVariableEntry[] = [];
            for (const [id, entry] of Array.from(this.outgoing.entries())) {
                if (entries.length >= SCRIPT_VARIABLES) {
                    break;
                }
                entries.push(entry);
                this.outgoing.delete(id);
            }
            this.tokens -= 1;
            const message: ScriptVariablesMessage = { t: MsgType.ScriptVariables, epoch: this.epoch > 0 ? this.epoch : 1, watch: [], entries, served: false };
            this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
            this.sent += entries.length;
        }
    }

    // a key joins the watch (false when it is there already or the watch is full)
    private addWatch(key: ScriptVariableKey): boolean {
        const id = ScriptVariablesService.keyId(key);
        if (this.watch.has(id)) {
            return false;
        }
        if (this.watch.size >= WATCH_MAX) {
            this.refused++;
            return false;
        }
        this.watch.set(id, { form: key.form, script: key.script, var: key.var });
        return true;
    }

    // the key's last value here equals the server's: nothing to write
    private isHere(id: string, entry: ScriptVariableEntry): boolean {
        const prev = this.last.get(id);
        return prev !== undefined && ScriptVariablesService.same(prev, ScriptVariablesService.valueOf(entry));
    }

    private summarize(now: number) {
        if (now - this.lastSummary < SUMMARY_MS) {
            return;
        }
        this.lastSummary = now;
        if (this.changed + this.sent + this.applied + this.hereAlready + this.refused + this.readRefused > 0) {
            logLab(TAG, `script variables in ${SUMMARY_MS / 1000} s: ${this.reads} read, ${this.changed} changed, ${this.sent} sent, ${this.applied} written (${this.appliedOwn} own), ${this.hereAlready} here already, ${this.refused + this.readRefused} refused`);
        }
        if (this.badAnswers > 0) {
            logError(this, `ReadScriptVariables or WriteScriptVariables answered ${this.badAnswers} unusable value(s) in ${SUMMARY_MS / 1000} s`);
        }
        this.reads = 0;
        this.changed = 0;
        this.sent = 0;
        this.applied = 0;
        this.appliedOwn = 0;
        this.hereAlready = 0;
        this.refused = 0;
        this.readRefused = 0;
        this.badAnswers = 0;
    }

    private enabled(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !s || s["scriptVariables"] !== false;
    }

    private persist() {
        this.sp.storage[WATCH_STORAGE_KEY] = { epoch: this.epoch, keys: Array.from(this.watch.values()) };
    }

    // After a plugin reload: the watch and the epoch the last login brought,
    // from storage; every key's baseline is taken anew
    private resume() {
        const stored = this.sp.storage[WATCH_STORAGE_KEY] as { epoch?: unknown; keys?: unknown } | undefined;
        if (!stored || typeof stored !== "object" || !Array.isArray(stored.keys)) {
            return;
        }
        for (const key of stored.keys) {
            if (ScriptVariablesService.isKey(key)) {
                this.addWatch(key);
            }
        }
        if (typeof stored.epoch === "number" && Number.isInteger(stored.epoch) && stored.epoch > 0) {
            this.epoch = stored.epoch;
        }
        if (this.watch.size > 0) {
            logTrace(this, `resumed watching ${this.watch.size} script variable(s) after a reload; their baselines are taken anew`);
        }
    }

    // rule 5: a key names a load-order form and two non-empty names
    private static isKey(x: unknown): x is ScriptVariableKey {
        if (!x || typeof x !== "object") {
            return false;
        }
        const k = x as Record<string, unknown>;
        return typeof k.form === "number" && Number.isInteger(k.form) && k.form > 0 && k.form <= LAST_GAME_FILE_ID
            && ScriptVariablesService.isName(k.script) && ScriptVariablesService.isName(k.var);
    }

    private static isName(x: unknown): x is string {
        return typeof x === "string" && x.length > 0 && x.indexOf("\u0000") < 0;
    }

    // a key with a kind 3 to 5 and that kind's payload
    private static isEntry(x: unknown): x is ScriptVariableEntry {
        if (!ScriptVariablesService.isKey(x)) {
            return false;
        }
        const e = x as unknown as Record<string, unknown>;
        switch (e.k) {
            case KIND_INT:
                return typeof e.i === "number" && Number.isInteger(e.i);
            case KIND_FLOAT:
                return typeof e.f === "number" && Number.isFinite(e.f);
            case KIND_BOOL:
                return typeof e.b === "boolean";
            default:
                return false;
        }
    }

    private static isRefusal(answer: unknown): boolean {
        if (answer === false || typeof answer === "string") {
            return true;
        }
        return !!answer && typeof answer === "object" && "error" in (answer as Record<string, unknown>);
    }

    // a key's identity: its form and its two names without case (Papyrus
    // compares names without case)
    private static keyId(k: ScriptVariableKey): string {
        return k.form.toString(16) + "|" + k.script.toLowerCase() + "|" + k.var.toLowerCase();
    }

    private static valueOf(e: ScriptVariableEntry): Value {
        return e.k === KIND_BOOL ? e.b === true : e.k === KIND_INT ? (e.i as number) : (e.f as number);
    }

    // a float holds 32 bits in the VM: compared at that precision
    private static same(a: Value, b: Value): boolean {
        if (typeof a === "number" && typeof b === "number") {
            return Math.fround(a) === Math.fround(b);
        }
        return a === b;
    }

    // the wire's entry: the key and its kind's payload alone
    private static wireEntry(e: ScriptVariableEntry): ScriptVariableEntry {
        const out: ScriptVariableEntry = { form: e.form, script: e.script, var: e.var, k: e.k };
        if (e.k === KIND_INT) {
            out.i = e.i;
        } else if (e.k === KIND_FLOAT) {
            out.f = e.f;
        } else {
            out.b = e.b;
        }
        return out;
    }

    // the keys read here, by keyId, in the serve's order
    private readonly watch = new Map<string, ScriptVariableKey>();
    // each key's last value here: its baseline, its last read, or the
    // server's value written here
    private readonly last = new Map<string, Value>();
    // the server's values to write, by key, the newest winning; this
    // player's own first
    private readonly toApply = new Map<string, ScriptVariableEntry>();
    private readonly toApplyOwn = new Map<string, ScriptVariableEntry>();
    // a read's changes waiting for the bucket, by key
    private readonly outgoing = new Map<string, ScriptVariableEntry>();
    // the read's keys as ReadScriptVariables takes them, rebuilt when the
    // watch grows
    private keysText: string | undefined;
    // the last epoch the server named on this connection
    private epoch = 0;
    private lastRead = 0;
    private readMissing = false;
    private writeMissing = false;
    private offSaid = false;
    // an own serve is being written: its line is due once it is through
    private ownServed = false;
    private ownWritten = 0;
    private ownRegistrations = 0;
    private tokens = REPORT_BURST;
    private refilledAt = Date.now();
    private lastSummary = Date.now();
    private reads = 0;
    private changed = 0;
    private sent = 0;
    private applied = 0;
    private appliedOwn = 0;
    private hereAlready = 0;
    private refused = 0;
    private readRefused = 0;
    private badAnswers = 0;
}
