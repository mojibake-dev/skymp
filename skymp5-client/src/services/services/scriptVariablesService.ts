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
// the wire's cap on one message's keys and entries, and the platform's on one
// call's (cap::SCRIPT_VARIABLES, kMaxVarKeys)
const SCRIPT_VARIABLES = 256;
// the server's values written per update (the plan's (d))
const APPLIES_PER_UPDATE = 32;
// the server's record holds at most this many keys (wire-rules
// script_variables VARIABLE_RECORD_CAP), and its login watch names the
// listed keys (the setting's, at most one record's cap: the census's 798)
// then the player's composed own keys (its quests' scalar variables and the
// two reserved names, bounded apart by the same cap), own keys last; the
// watch here holds both whole, and an own key is never dropped for a world
// key (addWatch)
const VARIABLE_RECORD_CAP = 1024;
const WATCH_MAX = 2 * VARIABLE_RECORD_CAP;
const SUMMARY_MS = 30_000;
// load-order forms end where the server's copies begin (the validator
// refuses a form of 0 or at 0xff000000 and up)
const LAST_GAME_FILE_ID = 0xfeffffff;
// rung 3's folded kinds, the ones a condition compares: int, float, bool
const KIND_INT = 3;
const KIND_FLOAT = 4;
const KIND_BOOL = 5;
// D-T2 B (docs/research/rung5-quests-2026-11-09.md 8(b)): the platform's two
// reserved names (ScriptState.cpp, ReadScriptVariables), which read a form's
// queued OnUpdateGameTime registration from SkyrimVM's
// queuedOnUpdateGameEvents as floats: the game hours until it fires, and a
// repeat's interval (0 for a single update). No Papyrus identifier holds a
// '<', so neither names a variable; WriteScriptVariables refuses both, and a
// relaunched seat registers again itself (reregister). Compared without case
const GAME_TIME_HOURS = "<OnUpdateGameTime>";
const GAME_TIME_REPEAT = "<OnUpdateGameTime:repeat>";
// the platform's refusal of both names when the form has no registration
// queued (ReadGameTimeRegistration's reason): this seat reports it as the
// hours' NO_REGISTRATION and a repeat of 0, so the record ends the
// registration too and a later login does not register it again
const NO_REGISTRATION_REASON = "no OnUpdateGameTime registration queued";
const NO_REGISTRATION = -1;
// the hours left count down as the game's days pass (the VM's clock moves in
// thousandths of a day, 0.024 game hours): a report goes when they rose (a
// new registration) or fell by this much since the last one sent, never at
// every step. UNMEASURED
const COUNTDOWN_STEP_HOURS = 0.25;
const COUNTDOWN_RISE_HOURS = 0.001;
// a single update overdue at the read (0 hours left) is registered again at
// this floor, so its interval stays positive (UNVERIFIED that 0 registers)
const SINGLE_FLOOR_HOURS = 0.01;
// The storage key of the watch (sp.storage survives a plugin reload, ADR-041's
// survivor): the serve comes once, at the login, and a hot-reloaded client
// keeps its connection and never logs in again
const WATCH_STORAGE_KEY = "thuum.scriptVariables.watch.v2";

type Value = number | boolean;

// a form's served registration, gathered from its two reserved own entries
type Registration = { script: string; hours?: number; repeat?: number };

// thuum docs/DECISIONS.md ADR-068 rung 5 (D-V1 B, D-T2 B; the plan's 1(d),
// docs/research/rung5-quests-2026-11-09.md; worldGlobalService.ts's shape):
// the variables dialogue conditions read follow the seat whose engine wrote
// them. A condition-read variable (CTDA 629 GetVMQuestVariable, 630
// GetVMScriptVariable; CommonLibSSE-NG TESCondition.h:762-763) is nobody's
// (D-F1 C), and most are written by a topic-info fragment that runs on the
// talking seat alone (458 of 770 keys, the plan's section 7: H-CV1 fails),
// so every seat reports a listed variable its engine changed, the latest
// write winning per key in the server's order (AliasFill 79's shape). R2:
// the writing seat's engine computes, the server records.
//
// The watch. The login's serve (served true) names the keys to read, a
// quest's or a reference's attached script variables by (form, script,
// var), each own or the world's (the player cut, D-T2 B); a key the server
// serves as this player's own is watched too. TESModPlatform.
// ReadScriptVariables(keysJson of [{form, script, var, own}], at most 256 a
// call, so the watch goes in chunks) reads them once a second, by name under
// the platform's attachedScriptsLock, answering per key, in the keys' order,
// {form, script, var, k, i|f|b, own?} or {form, script, var, reason} ("[]"
// for a bad call). A key's first read here is its
// baseline, never a report, so the save's or the template's value is never
// sent over the world's; after it, a change goes out as ScriptVariables
// {watch [], entries, served false}, each entry with its own flag and seq 0,
// through the validator's bucket, at most 256 entries a message. A key whose
// served or relayed value is still to be written is not compared, and a
// value written here from the server is the key's baseline, so nothing
// applied is reported back.
//
// The receivers. A relay and the login's serve are held by key, the newest
// winning (an entry whose server sequence is not above the newest taken for
// its key is stale), and written through TESModPlatform.WriteScriptVariables
// (entriesJson of [{form, script, var, k, i|f|b}]; Variable's setters,
// refusing a kind other than the declared), APPLIES_PER_UPDATE at most an
// update, answered per entry {form, script, var, written, reason?} ("[]" for a
// bad call, every entry then unwritten). D-T2 B: the serve's own entries,
// this player's own quests' state (the marked quests' scalar variables:
// PlayerVampireQuest's VampireStatus, LastFeedTime, FeedTimer), are written
// first, before the watch's first read here (the gate: no report of this
// seat's fresh values over its own record). Their game-time registration
// arrives as the two reserved names, never written: once the own entries are
// through, each form whose served hours are not NO_REGISTRATION and which
// has no registration queued here (a reconnect with the game up keeps its
// engine's) registers again, by the served repeat,
// Form.RegisterForUpdateGameTime(repeat) or, for a single update,
// Form.RegisterForSingleUpdateGameTime(hours). Rule 5: a form must be a
// load-order id, the names non-empty, the kind 3 to 5 with its payload, a
// float finite.
//
// callNative needs the Papyrus VM's context (Skyrim Platform refuses natives
// in a message handler): a message is kept and read on the next update. An
// older platform without the natives throws there: said once, and this seat
// reads or writes nothing. The client setting scriptVariables false turns
// the service off.
//
// Lines for the lab (verb:quest-stage): "own state: <n> served value(s)
// written for this player, <m> game-time registration(s) registered again,
// <q> queued here already" once a serve, and the 30 s summary "script
// variables in 30 s: <r> read, <c> changed, <s> sent, <a> written (<o> own),
// <h> here already, <x> stale, <y> refused".
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
        const watch = Array.isArray(m.watch) ? m.watch : [];
        let added = 0;
        for (const key of watch) {
            if (!ScriptVariablesService.isKey(key)) {
                this.refused++;
                continue;
            }
            if (this.addWatch(key, key.own === true)) {
                added++;
            }
        }
        const entries = Array.isArray(m.entries) ? m.entries : [];
        for (const entry of entries) {
            if (!ScriptVariablesService.isEntry(entry)) {
                this.refused++;
                continue;
            }
            const id = ScriptVariablesService.keyId(entry);
            if (this.isStale(id, entry.seq)) {
                this.stale++;
                continue;
            }
            if (entry.own === true) {
                // this player's own state, written first
                this.toApply.delete(id);
                this.toApplyOwn.delete(id);
                this.toApplyOwn.set(id, entry);
                this.ownServed = true;
                if (this.addWatch(entry, true)) {
                    added++;
                }
                continue;
            }
            if (this.toApplyOwn.has(id)) {
                continue; // this player's own value wins
            }
            this.toApply.delete(id);
            this.toApply.set(id, entry);
        }
        if (added > 0) {
            this.chunks = undefined;
            this.persist();
            if (m.served) {
                logTrace(this, `watching ${this.watch.size} script variable(s), ${added} new from the serve`);
            }
        }
        this.sayCuts();
    }

    // a new connection (connectionAccepted, ahead of all its messages): the
    // server serves the watch and the record anew
    private onConnectionAccepted() {
        this.watch.clear();
        this.last.clear();
        this.lastSeq.clear();
        this.toApply.clear();
        this.toApplyOwn.clear();
        this.outgoing.clear();
        this.registrations.clear();
        this.chunks = undefined;
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
        // the own state first (the gate): no read before it is written and
        // its registrations are registered again
        if (this.watch.size > 0 && this.toApplyOwn.size === 0 && !this.readMissing && now - this.lastRead >= READ_MS) {
            this.lastRead = now;
            this.collect(this.readWatch());
        }
        if (this.outgoing.size > 0) {
            this.send(now);
        }
        this.summarize(now);
    }

    // the server's values, a bounded number an update, this player's own
    // first; a value equal to the key's last here is not written again; the
    // own registrations gathered and, once the own entries are through,
    // registered again
    private applySome() {
        const batch: ScriptVariableEntry[] = [];
        let fromOwn = 0;
        for (const [id, entry] of Array.from(this.toApplyOwn.entries())) {
            if (batch.length >= APPLIES_PER_UPDATE) {
                break;
            }
            this.toApplyOwn.delete(id);
            if (ScriptVariablesService.isGameTimeName(entry.var)) {
                this.gather(entry);
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
            if (ScriptVariablesService.isGameTimeName(entry.var)) {
                this.refused++; // a registration is a player's own, never the world's
                continue;
            }
            if (this.isHere(id, entry)) {
                this.hereAlready++;
                continue;
            }
            batch.push(entry);
        }
        if (batch.length > 0) {
            this.write(batch, fromOwn);
        }
        if (this.toApplyOwn.size === 0 && !this.writeMissing) {
            if (this.registrations.size > 0) {
                this.reregister();
            }
            if (this.ownServed) {
                this.ownServed = false;
                logLab(TAG, `own state: ${this.ownWritten} served value(s) written for this player, ${this.ownRegistered} game-time registration(s) registered again, ${this.ownKept} queued here already`);
                this.ownWritten = 0;
                this.ownRegistered = 0;
                this.ownKept = 0;
            }
        }
    }

    // WriteScriptVariables for a batch; an entry the native answers written
    // becomes its key's last value here, so the next read does not report it
    // back; "[]" or an answer of another length writes nothing
    private write(batch: ScriptVariableEntry[], fromOwn: number) {
        let raw: unknown;
        try {
            raw = this.sp.callNative("TESModPlatform", "WriteScriptVariables", undefined, JSON.stringify(batch.map(ScriptVariablesService.writeEntry)));
        } catch (e) {
            this.writeMissing = true;
            this.toApply.clear();
            this.toApplyOwn.clear();
            this.registrations.clear();
            logLab(TAG, `WriteScriptVariables unavailable on this platform (${e}); no script variable is written here`);
            return;
        }
        const answers = ScriptVariablesService.answerList(raw, batch.length);
        if (!answers) {
            this.badAnswers++;
            this.refused += batch.length;
            return;
        }
        batch.forEach((entry, n) => {
            const answer = answers[n] as Record<string, unknown> | null;
            if (!answer || typeof answer !== "object" || answer["written"] !== true) {
                this.refused++; // the reason is in the platform's line
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

    // a served own entry on one of the reserved names, into its form's
    // registration
    private gather(entry: ScriptVariableEntry) {
        if (entry.k !== KIND_FLOAT || typeof entry.f !== "number") {
            this.refused++;
            return;
        }
        const r = this.registrations.get(entry.form) ?? { script: entry.script };
        if (entry.var.toLowerCase() === GAME_TIME_HOURS.toLowerCase()) {
            r.hours = entry.f;
        } else {
            r.repeat = entry.f;
        }
        this.registrations.set(entry.form, r);
    }

    // D-T2 B: each served registration registered again on its form, by its
    // repeat, unless this game queues one for the form already (read through
    // the hours' name first: a reconnect with the game up keeps the engine's)
    // or the record ended it (hours NO_REGISTRATION)
    private reregister() {
        const wanted = Array.from(this.registrations.entries())
            .filter(([, r]) => typeof r.hours === "number" && r.hours >= 0);
        this.registrations.clear();
        if (wanted.length === 0) {
            return;
        }
        const keys: ScriptVariableKey[] = wanted.map(([form, r]) => ({ form, script: r.script, var: GAME_TIME_HOURS, own: true }));
        const here: ScriptVariableEntry[] = [];
        for (let at = 0; at < keys.length; at += SCRIPT_VARIABLES) {
            const chunk = keys.slice(at, at + SCRIPT_VARIABLES);
            const values = this.read(chunk, JSON.stringify(chunk.map(ScriptVariablesService.readKey)), false);
            if (!values) {
                return;
            }
            here.push(...values);
        }
        // a form whose hours read a value has one queued; NO_REGISTRATION is none
        const queued = new Set(here.filter((e) => typeof e.f === "number" && e.f >= 0).map((e) => e.form));
        for (const [formId, r] of wanted) {
            if (queued.has(formId)) {
                this.ownKept++;
                continue;
            }
            const form = this.sp.Form.from(this.sp.Game.getFormEx(formId));
            if (!form) {
                this.refused++;
                logError(this, `the served game-time registration of ${formId.toString(16)} names no form here`);
                continue;
            }
            const repeat = typeof r.repeat === "number" && Number.isFinite(r.repeat) ? r.repeat : 0;
            if (repeat > 0) {
                form.registerForUpdateGameTime(repeat);
                logTrace(this, `the game-time registration of ${formId.toString(16)} registered again: every ${repeat} game hour(s)`);
            } else {
                const hours = Math.max(r.hours as number, SINGLE_FLOOR_HOURS);
                form.registerForSingleUpdateGameTime(hours);
                logTrace(this, `the game-time registration of ${formId.toString(16)} registered again: once, in ${hours} game hour(s)`);
            }
            this.ownRegistered++;
        }
    }

    // the watch read whole, in chunks of SCRIPT_VARIABLES keys (the
    // platform's bound on one call), their texts built once per watch;
    // undefined when a chunk's call answered nothing usable
    private readWatch(): ScriptVariableEntry[] | undefined {
        if (this.chunks === undefined) {
            const keys = Array.from(this.watch.values());
            this.chunks = [];
            for (let at = 0; at < keys.length; at += SCRIPT_VARIABLES) {
                const chunk = keys.slice(at, at + SCRIPT_VARIABLES);
                this.chunks.push({ keys: chunk, text: JSON.stringify(chunk.map(ScriptVariablesService.readKey)) });
            }
        }
        const out: ScriptVariableEntry[] = [];
        for (const chunk of this.chunks) {
            const values = this.read(chunk.keys, chunk.text, true);
            if (!values) {
                return undefined;
            }
            out.push(...values);
        }
        this.reads++;
        return out;
    }

    // one ReadScriptVariables call over at most SCRIPT_VARIABLES keys (`text`
    // their JSON); undefined when the native is gone or answered nothing
    // usable. The values read, a reserved name's "no registration" as
    // NO_REGISTRATION (the hours) or 0 (the repeat); the other refusals
    // counted (the platform's line has the reason) when `watched`
    private read(keys: ScriptVariableKey[], text: string, watched: boolean): ScriptVariableEntry[] | undefined {
        let raw: unknown;
        try {
            raw = this.sp.callNative("TESModPlatform", "ReadScriptVariables", undefined, text);
        } catch (e) {
            this.readMissing = true;
            logLab(TAG, `ReadScriptVariables unavailable on this platform (${e}); this seat reports no script variable`);
            return undefined;
        }
        const answers = ScriptVariablesService.answerList(raw, keys.length);
        if (!answers) {
            this.badAnswers++;
            return undefined;
        }
        const out: ScriptVariableEntry[] = [];
        answers.forEach((answer, n) => {
            const key = keys[n];
            const a = answer as Record<string, unknown> | null;
            const kind = a && typeof a === "object" ? a["k"] : undefined;
            if (kind !== undefined) {
                const value: ScriptVariableEntry = { form: key.form, script: key.script, var: key.var, own: key.own, k: kind as number, seq: 0 };
                if (kind === KIND_INT) {
                    value.i = a!["i"] as number;
                } else if (kind === KIND_FLOAT) {
                    value.f = a!["f"] as number;
                } else {
                    value.b = a!["b"] as boolean;
                }
                if (ScriptVariablesService.isEntry(value)) {
                    out.push(value);
                    return;
                }
            }
            const reason = a && typeof a === "object" ? a["reason"] : undefined;
            if (ScriptVariablesService.isGameTimeName(key.var) && typeof reason === "string" && reason.indexOf(NO_REGISTRATION_REASON) === 0) {
                const hours = key.var.toLowerCase() === GAME_TIME_HOURS.toLowerCase();
                out.push({ form: key.form, script: key.script, var: key.var, own: key.own, k: KIND_FLOAT, f: hours ? NO_REGISTRATION : 0, seq: 0 });
                return;
            }
            if (watched) {
                this.readRefused++;
            }
        });
        return out;
    }

    // a read's values against each key's last here: a key's first read is its
    // baseline; a change goes out, the latest per key (the hours left only
    // when they rose or fell by COUNTDOWN_STEP_HOURS since the last sent); a
    // key with a server value still to write is not compared
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
            if (prev !== undefined) {
                const moved = entry.var.toLowerCase() === GAME_TIME_HOURS.toLowerCase()
                    ? ScriptVariablesService.countdownMoved(prev, value)
                    : !ScriptVariablesService.same(prev, value);
                if (!moved) {
                    continue;
                }
            }
            this.last.set(id, value);
            if (prev === undefined) {
                continue;
            }
            this.changed++;
            this.outgoing.delete(id);
            this.outgoing.set(id, ScriptVariablesService.reportEntry(entry, this.watch.get(id)?.own === true));
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
            const message: ScriptVariablesMessage = { t: MsgType.ScriptVariables, watch: [], entries, served: false };
            this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
            this.sent += entries.length;
        }
    }

    // a key joins the watch, own or the world's (false when it is there
    // already, an own flag then kept, or the watch is full). Own keys are
    // never dropped for world keys: a full watch lets its last world key go
    // for an own one; every cut is counted for sayCuts
    private addWatch(key: ScriptVariableKey, own: boolean): boolean {
        const id = ScriptVariablesService.keyId(key);
        const had = this.watch.get(id);
        if (had) {
            if (own && !had.own) {
                had.own = true;
                this.chunks = undefined;
            }
            return false;
        }
        if (this.watch.size >= WATCH_MAX && !(own && this.letWorldKeyGo())) {
            this.cutKeys++;
            if (own) {
                this.cutOwn++;
            }
            return false;
        }
        this.watch.set(id, { form: key.form, script: key.script, var: key.var, own });
        return true;
    }

    // the last world key out of a full watch, for an own key; false when the
    // watch holds own keys alone
    private letWorldKeyGo(): boolean {
        let lastWorld: string | undefined;
        this.watch.forEach((k, id) => {
            if (!k.own) {
                lastWorld = id;
            }
        });
        if (lastWorld === undefined) {
            return false;
        }
        this.watch.delete(lastWorld);
        this.last.delete(lastWorld);
        this.chunks = undefined;
        this.cutForOwn++;
        return true;
    }

    // one line for a message (or a reload) whose keys a full watch cut
    private sayCuts() {
        if (this.cutKeys + this.cutForOwn === 0) {
            return;
        }
        logLab(TAG, `the script variable watch is full at ${WATCH_MAX} keys: ${this.cutKeys} key(s) not watched (${this.cutOwn} of them own), ${this.cutForOwn} world key(s) let go for own keys`);
        this.cutKeys = 0;
        this.cutOwn = 0;
        this.cutForOwn = 0;
    }

    // the server's sequence for a key (the record's, rising at each recorded
    // write): an entry not above the newest taken here is stale; 0 (a report
    // relayed by an older server) is in arrival order
    private isStale(id: string, seq: unknown): boolean {
        if (typeof seq !== "number" || !Number.isInteger(seq) || seq <= 0) {
            return false;
        }
        const had = this.lastSeq.get(id);
        if (had !== undefined && seq <= had) {
            return true;
        }
        this.lastSeq.set(id, seq);
        return false;
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
        if (this.changed + this.sent + this.applied + this.hereAlready + this.stale + this.refused + this.readRefused > 0) {
            logLab(TAG, `script variables in ${SUMMARY_MS / 1000} s: ${this.reads} read, ${this.changed} changed, ${this.sent} sent, ${this.applied} written (${this.appliedOwn} own), ${this.hereAlready} here already, ${this.stale} stale, ${this.refused + this.readRefused} refused`);
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
        this.stale = 0;
        this.refused = 0;
        this.readRefused = 0;
        this.badAnswers = 0;
    }

    private enabled(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !s || s["scriptVariables"] !== false;
    }

    private persist() {
        this.sp.storage[WATCH_STORAGE_KEY] = { keys: Array.from(this.watch.values()) };
    }

    // After a plugin reload: the watch the last login brought, from storage;
    // every key's baseline is taken anew
    private resume() {
        const stored = this.sp.storage[WATCH_STORAGE_KEY] as { keys?: unknown } | undefined;
        if (!stored || typeof stored !== "object" || !Array.isArray(stored.keys)) {
            return;
        }
        for (const key of stored.keys) {
            if (ScriptVariablesService.isKey(key)) {
                this.addWatch(key, key.own === true);
            }
        }
        this.sayCuts();
        if (this.watch.size > 0) {
            logTrace(this, `resumed watching ${this.watch.size} script variable(s) after a reload; their baselines are taken anew`);
        }
    }

    // a native's answer: a JSON array of `length` elements, else undefined
    // (the platform's "[]" for a bad call among them)
    private static answerList(raw: unknown, length: number): unknown[] | undefined {
        if (typeof raw !== "string") {
            return undefined;
        }
        try {
            const parsed: unknown = JSON.parse(raw);
            return Array.isArray(parsed) && parsed.length === length ? parsed : undefined;
        } catch (e) {
            return undefined;
        }
    }

    private static isGameTimeName(name: string): boolean {
        const lower = name.toLowerCase();
        return lower === GAME_TIME_HOURS.toLowerCase() || lower === GAME_TIME_REPEAT.toLowerCase();
    }

    // the hours left moved enough for a report: rose (a new registration) or
    // fell by a step (NO_REGISTRATION, the end, falls by more than one)
    private static countdownMoved(prev: Value, value: Value): boolean {
        if (typeof prev !== "number" || typeof value !== "number") {
            return prev !== value;
        }
        return value > prev + COUNTDOWN_RISE_HOURS || prev - value >= COUNTDOWN_STEP_HOURS;
    }

    // rule 5: a key names a load-order form and two non-empty names, its own
    // flag a boolean when present (absent: the world's)
    private static isKey(x: unknown): x is ScriptVariableKey {
        if (!x || typeof x !== "object") {
            return false;
        }
        const k = x as Record<string, unknown>;
        return typeof k.form === "number" && Number.isInteger(k.form) && k.form > 0 && k.form <= LAST_GAME_FILE_ID
            && ScriptVariablesService.isName(k.script) && ScriptVariablesService.isName(k.var)
            && (k.own === undefined || typeof k.own === "boolean");
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

    // ReadScriptVariables' key: the key's three names and its own flag
    private static readKey(k: ScriptVariableKey): ScriptVariableKey {
        return { form: k.form, script: k.script, var: k.var, own: k.own };
    }

    // WriteScriptVariables' entry: the key and exactly its kind's payload
    private static writeEntry(e: ScriptVariableEntry): Record<string, unknown> {
        const out: Record<string, unknown> = { form: e.form, script: e.script, var: e.var, k: e.k };
        if (e.k === KIND_INT) {
            out.i = e.i;
        } else if (e.k === KIND_FLOAT) {
            out.f = e.f;
        } else {
            out.b = e.b;
        }
        return out;
    }

    // the wire's report entry: the key with its own flag, exactly its kind's
    // payload, and seq 0 (the server's sequence is never a report's)
    private static reportEntry(e: ScriptVariableEntry, own: boolean): ScriptVariableEntry {
        const out: ScriptVariableEntry = { form: e.form, script: e.script, var: e.var, own, k: e.k, seq: 0 };
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
    // each key's last value here: its baseline, its last read (the hours
    // left: its last sent), or the server's value written here
    private readonly last = new Map<string, Value>();
    // the newest server sequence taken for each key
    private readonly lastSeq = new Map<string, number>();
    // the server's values to write, by key, the newest winning; this
    // player's own first
    private readonly toApply = new Map<string, ScriptVariableEntry>();
    private readonly toApplyOwn = new Map<string, ScriptVariableEntry>();
    // the served registrations to register again, by form
    private readonly registrations = new Map<number, Registration>();
    // a read's changes waiting for the bucket, by key
    private readonly outgoing = new Map<string, ScriptVariableEntry>();
    // the watch's keys as ReadScriptVariables takes them, in chunks of
    // SCRIPT_VARIABLES, rebuilt when the watch changes
    private chunks: Array<{ keys: ScriptVariableKey[]; text: string }> | undefined;
    private lastRead = 0;
    private readMissing = false;
    private writeMissing = false;
    private offSaid = false;
    // an own serve is being written: its line is due once it is through
    private ownServed = false;
    private ownWritten = 0;
    private ownRegistered = 0;
    private ownKept = 0;
    private tokens = REPORT_BURST;
    private refilledAt = Date.now();
    private lastSummary = Date.now();
    private reads = 0;
    private changed = 0;
    private sent = 0;
    private applied = 0;
    private appliedOwn = 0;
    private hereAlready = 0;
    private stale = 0;
    private refused = 0;
    private readRefused = 0;
    private badAnswers = 0;
    // the watch's cuts since the last line: keys not watched (own among
    // them), world keys let go for own keys
    private cutKeys = 0;
    private cutOwn = 0;
    private cutForOwn = 0;
}
