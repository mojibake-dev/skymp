import { MsgType } from "../../messages";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { WorldGlobalMessage } from "../messages/worldGlobalMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { PlayerGlobalsService } from "./playerGlobalsService";
import { clockGlobalIds } from "./timeService";

const TAG = "verb:world-globals";
// How often the streamer diffs the GLOB array, ms: the validator takes one
// report a second after a burst of 4 (wire-validate's WorldGlobal row, the
// plan's (b), UNMEASURED), mirrored as a bucket so a diff past one report's
// entries waits here instead of being refused there
const DIFF_MS = 1000;
const REPORT_BURST = 4;
const REPORT_PER_S = 1;
// the wire's cap on one message's entries (wire-schema's WORLD_GLOBALS)
const WORLD_GLOBALS = 512;
// the server's values set per update (the plan's (d))
const APPLIES_PER_UPDATE = 32;
const SUMMARY_MS = 30_000;
// load-order forms end where the server's copies begin (the validator refuses
// a global of 0 or at 0xff000000 and up)
const LAST_GAME_FILE_ID = 0xfeffffff;
// The storage key of the stream's epoch (sp.storage survives a plugin reload,
// ADR-041's survivor; PlayerGlobalsService's note): a hot-reloaded streamer
// keeps its connection and its election, and the stream word never comes
// again
const STREAM_STORAGE_KEY = "worldGlobalsStream";

// thuum docs/DECISIONS.md ADR-068 rung 4 and amendment B (the state rows,
// WorldGlobal 80; docs/research/rung4-state-rows-2026-11-07.md (d);
// playerGlobalsService.ts's shape): the world's global variables, the ones no
// player owns, follow one seat's engine. A global is nobody's (D-F1 C): every
// seat's scripts write it, and one streaming seat, elected by the server from
// ADR-066 decision 6's eligibility (wire-rules world_streamer.rs: past its
// first movement report, not stale, not dead; one seat at an epoch), reports
// the changes its engine sees. R2: the streamer computes, the server records.
//
// The streamer. The server's stream word (stream true at an epoch) starts the
// watch: TESModPlatform.DiffWorldGlobals(true, exclude) takes the baseline,
// then once a second DiffWorldGlobals(false, exclude) answers the globals
// whose values moved since its last call, [[id, value], ...], sent as
// WorldGlobal with the word's epoch, at most WORLD_GLOBALS entries a message.
// Excluded: the clock's six (TimeService's, the server's own word) and the
// globals PlayerGlobalsService watches (each player's own). The baseline
// waits for the values the server served before it (the login's record), so
// none of them is reported back; a value set here after the baseline is not
// reported back either (the next diff names it). A stream word with stream
// false, or a new connection, ends the stream.
//
// The receivers. A relay of the streamer's report and the login's record
// (served) set each value through GlobalVariable.setValue on the update,
// APPLIES_PER_UPDATE at most an update, the newest per global winning. Rule 5: the
// global must be a GlobalVariable here, never one of the excluded, its value
// finite.
//
// callNative and the GlobalVariable natives need the Papyrus VM's context
// (Skyrim Platform refuses natives in a message handler): a message is kept
// and read on the next update. An older platform without DiffWorldGlobals
// throws there: said once, and this seat streams nothing.
//
// Lines for the lab (verb:world-globals): "world globals: this seat streams
// at epoch <e>", "world globals: the stream ends at epoch <e>", the
// streamer's 30 s summary "world globals: <n> changed, <m> sent", and a
// receiver's "world globals applied in 30 s: ...".
export class WorldGlobalService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("worldGlobalMessage", (e) => this.onMessage(e));
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.controller.on("update", () => this.onUpdate());
        this.resume();
    }

    // a stream word, a relay or the login's record (a message handler: JS
    // state alone, applied on the update)
    private onMessage(e: ConnectionMessage<WorldGlobalMessage>) {
        const m = e.message;
        if (typeof m.stream === "boolean") {
            this.onStreamWord(m.stream, m.epoch);
        }
        const entries = Array.isArray(m.entries) ? m.entries : [];
        for (const entry of entries) {
            const id = entry ? entry.global : undefined;
            const value = entry ? entry.value : undefined;
            if (typeof id !== "number" || !Number.isInteger(id) || id <= 0 || id > LAST_GAME_FILE_ID
                || typeof value !== "number" || !Number.isFinite(value)) {
                this.refused++;
                continue;
            }
            this.toApply.delete(id);
            this.toApply.set(id, value);
        }
    }

    // the server's election: a word older than the last one this connection
    // brought is ignored (ADR-066's fence, as HostService ignores an old
    // grant)
    private onStreamWord(stream: boolean, rawEpoch: unknown) {
        const epoch = typeof rawEpoch === "number" && Number.isInteger(rawEpoch) && rawEpoch > 0 ? rawEpoch : 0;
        if (epoch === 0 || epoch < this.lastWordEpoch) {
            logTrace(this, `stream word ${stream} at epoch ${String(rawEpoch)} ignored: epoch ${this.lastWordEpoch} seen`);
            return;
        }
        this.lastWordEpoch = epoch;
        if (stream) {
            if (this.streamEpoch !== epoch) {
                this.startStream(epoch);
                logLab(TAG, `world globals: this seat streams at epoch ${epoch}`);
            }
        } else if (this.streamEpoch !== 0) {
            this.stopStream();
            logLab(TAG, `world globals: the stream ends at epoch ${epoch}`);
        }
    }

    private startStream(epoch: number) {
        this.streamEpoch = epoch;
        this.baselineDue = true;
        this.outgoing.clear();
        this.appliedHere.clear();
        this.sp.storage[STREAM_STORAGE_KEY] = { epoch };
    }

    private stopStream() {
        this.streamEpoch = 0;
        this.baselineDue = false;
        this.outgoing.clear();
        this.appliedHere.clear();
        this.sp.storage[STREAM_STORAGE_KEY] = { epoch: 0 };
    }

    // a new connection (connectionAccepted, ahead of all its messages): the
    // server elects and serves anew
    private onConnectionAccepted() {
        if (this.streamEpoch !== 0) {
            logTrace(this, `the stream at epoch ${this.streamEpoch} ends with its connection`);
        }
        this.stopStream();
        this.lastWordEpoch = 0;
        this.toApply.clear();
    }

    private onUpdate() {
        const now = Date.now();
        if (this.toApply.size > 0) {
            this.applySome();
        }
        if (this.streamEpoch !== 0 && !this.nativeMissing) {
            if (this.baselineDue) {
                // the server's values first (the login's record), then the
                // baseline, so none of them is reported back
                if (this.toApply.size === 0 && this.diff(true) !== undefined) {
                    this.baselineDue = false;
                    this.lastDiff = now;
                    logTrace(this, `the GLOB array's baseline taken at epoch ${this.streamEpoch}, excluding ${this.excluded().size} global(s)`);
                }
            } else if (now - this.lastDiff >= DIFF_MS) {
                this.lastDiff = now;
                this.collect(this.diff(false));
            }
            if (this.outgoing.size > 0) {
                this.send(now);
            }
        }
        this.summarize(now);
    }

    // the server's values, a bounded number an update
    private applySome() {
        const excluded = this.excluded();
        let n = 0;
        for (const [id, value] of Array.from(this.toApply.entries())) {
            if (n >= APPLIES_PER_UPDATE) {
                return;
            }
            n++;
            this.toApply.delete(id);
            if (excluded.has(id)) {
                // the clock is TimeService's, a player's own globals
                // PlayerGlobalsService's
                this.refused++;
                continue;
            }
            const global = this.sp.GlobalVariable.from(this.sp.Game.getFormEx(id));
            if (!global) {
                this.refused++;
                continue;
            }
            // a global holds a 32-bit float: compared at that precision
            if (Math.fround(global.getValue()) === Math.fround(value)) {
                this.hereAlready++;
                continue;
            }
            global.setValue(value);
            this.applied++;
            if (this.streamEpoch !== 0 && !this.baselineDue) {
                // set after the baseline: the next diff names it, and it is
                // the server's already
                this.appliedHere.set(id, value);
            }
        }
    }

    // DiffWorldGlobals's answer, the pairs that fit the wire; undefined when
    // the native is gone or answered nothing usable. reset takes the baseline
    // and answers []
    private diff(reset: boolean): Array<[number, number]> | undefined {
        let raw: unknown;
        try {
            raw = this.sp.callNative("TESModPlatform", "DiffWorldGlobals", undefined, reset, this.excludeText());
        } catch (e) {
            this.nativeMissing = true;
            logLab(TAG, `DiffWorldGlobals unavailable on this platform (${e}); this seat streams no world global`);
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
        const out: Array<[number, number]> = [];
        for (const pair of parsed) {
            if (!Array.isArray(pair) || pair.length !== 2) {
                this.badAnswers++;
                continue;
            }
            const id = pair[0];
            const value = pair[1];
            if (typeof id !== "number" || !Number.isInteger(id) || id <= 0 || id > LAST_GAME_FILE_ID
                || typeof value !== "number" || !Number.isFinite(value)) {
                this.badAnswers++;
                continue;
            }
            out.push([id, value]);
        }
        return out;
    }

    // a diff's changes into the outgoing set, the latest per global, less the
    // values this seat set from the server's word after the baseline and the
    // excluded ids (the native skips them; checked again here, so a refusal
    // of a whole report never waits on the platform's parse)
    private collect(pairs: Array<[number, number]> | undefined) {
        if (!pairs || pairs.length === 0) {
            return;
        }
        const excluded = this.excluded();
        for (const [id, value] of pairs) {
            this.changed++;
            const set = this.appliedHere.get(id);
            if (set !== undefined) {
                this.appliedHere.delete(id);
                if (Math.fround(set) === Math.fround(value)) {
                    continue;
                }
            }
            if (excluded.has(id)) {
                continue;
            }
            this.outgoing.delete(id);
            this.outgoing.set(id, value);
        }
    }

    private send(now: number) {
        this.tokens = Math.min(REPORT_BURST, this.tokens + ((now - this.refilledAt) / 1000) * REPORT_PER_S);
        this.refilledAt = now;
        while (this.outgoing.size > 0 && this.tokens >= 1) {
            const entries: Array<{ global: number; value: number }> = [];
            for (const [global, value] of Array.from(this.outgoing.entries())) {
                if (entries.length >= WORLD_GLOBALS) {
                    break;
                }
                entries.push({ global, value });
                this.outgoing.delete(global);
            }
            this.tokens -= 1;
            const message: WorldGlobalMessage = { t: MsgType.WorldGlobal, epoch: this.streamEpoch, entries, served: false };
            this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
            this.sent += entries.length;
        }
    }

    // the ids the stream never carries: the clock's six and the player
    // globals watch, read live (the watch comes at the login)
    private excluded(): Set<number> {
        const watched = this.controller.lookupListener(PlayerGlobalsService).watched();
        const key = watched.join(",");
        if (key !== this.excludedKey) {
            this.excludedKey = key;
            this.excludedIds = new Set<number>([...clockGlobalIds, ...watched]);
            this.excludedText = Array.from(this.excludedIds).map((id) => id.toString(16)).join(",");
        }
        return this.excludedIds;
    }

    // DiffWorldGlobals's exclude: the excluded ids in hex without a prefix,
    // comma-separated ("35,36,37,38,39,3a,...")
    private excludeText(): string {
        this.excluded();
        return this.excludedText;
    }

    // the window's counts: the streamer's line every window it streams (the
    // rate probe's read, H-WG1), a receiver's when it set anything
    private summarize(now: number) {
        if (now - this.lastSummary < SUMMARY_MS) {
            return;
        }
        this.lastSummary = now;
        if (this.streamEpoch !== 0) {
            logLab(TAG, `world globals: ${this.changed} changed, ${this.sent} sent`);
        }
        if (this.applied + this.hereAlready + this.refused > 0) {
            logLab(TAG, `world globals applied in ${SUMMARY_MS / 1000} s: ${this.applied} set, ${this.hereAlready} here already, ${this.refused} refused`);
        }
        if (this.badAnswers > 0) {
            logError(this, `DiffWorldGlobals answered ${this.badAnswers} unusable value(s) in ${SUMMARY_MS / 1000} s`);
        }
        this.changed = 0;
        this.sent = 0;
        this.applied = 0;
        this.hereAlready = 0;
        this.refused = 0;
        this.badAnswers = 0;
    }

    // After a plugin reload: the stream the last plugin held, from storage;
    // the baseline is taken anew
    private resume() {
        const stored = this.sp.storage[STREAM_STORAGE_KEY] as { epoch?: unknown } | undefined;
        const epoch = stored && typeof stored === "object" ? stored.epoch : undefined;
        if (typeof epoch !== "number" || !Number.isInteger(epoch) || epoch <= 0) {
            return;
        }
        this.lastWordEpoch = epoch;
        this.startStream(epoch);
        logTrace(this, `resumed streaming at epoch ${epoch} after a reload; the baseline is taken anew`);
    }

    // this seat streams at this epoch; 0 for none
    private streamEpoch = 0;
    // the highest stream word's epoch this connection brought
    private lastWordEpoch = 0;
    private baselineDue = false;
    private lastDiff = 0;
    private nativeMissing = false;
    // the server's values to set, by global, the newest winning
    private readonly toApply = new Map<number, number>();
    // the values set here from the server's word while streaming, which the
    // next diff names and this seat does not report back
    private readonly appliedHere = new Map<number, number>();
    // the diff's changes waiting for the bucket, by global
    private readonly outgoing = new Map<number, number>();
    private excludedKey: string | undefined;
    private excludedIds = new Set<number>();
    private excludedText = "";
    private tokens = REPORT_BURST;
    private refilledAt = Date.now();
    private lastSummary = Date.now();
    private changed = 0;
    private sent = 0;
    private applied = 0;
    private hereAlready = 0;
    private refused = 0;
    private badAnswers = 0;
}
