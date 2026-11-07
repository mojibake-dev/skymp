import { ScriptCall } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { AliasFillMessage } from "../messages/aliasFillMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// a scriptCalls row of ReferenceAlias.ForceRefTo or Clear as rung 4's
// platform batches it (thuum docs/research/rung4-state-rows-2026-11-07.md
// (a), "Everyone rows"): its self the alias's quest (the handle's low 32
// bits, EveryoneSelfId) and the platform's coming field `alias`, the alias's
// id ((handle >> 32) & 0xFFFF of a kind-0 handle that is not a plain form's,
// H-AF3), which ScriptCall gains in Definitions.txt and skyrimPlatform.ts
// from the platform layer; read here through this local extension until the
// client's typing carries it. Absent from an older platform: no report
type AliasCall = ScriptCall & { alias?: number };

type Fill = {
    quest: number;
    alias: number;
    refr: number;     // 0 for a cleared alias
    served: boolean;
    why: string;      // the reason it waits, for the summary's trace
    parkedAt?: number; // when it was first parked, for PARKED_HOLD_MS
};

const TAG = "verb:quest-stage";
const PLAYER_ID = 0x14;
// game-file references end where the server's copies begin; the validator
// refuses a quest or a refr at 0xff000000 and up, an alias past 0xFFFF
const LAST_GAME_FILE_ID = 0xfeffffff;
const ALIAS_MAX = 0xffff;
// the validator's budget (wire-validate's AliasFill row: 16 at once, then 4 a
// second, UNMEASURED; the plan's (b)), mirrored as a bucket so a burst of
// reports waits here instead of being refused there (TrapResultService's
// shape): 57 ForceRefTo were measured at one guest's arrival
// (docs/DECISIONS.md, ADR-068's census)
const REPORT_BURST = 16;
const REPORT_PER_S = 4;
const REPORTS_MAX = 256;
// the server's words applied per update (UNMEASURED)
const APPLIES_PER_UPDATE = 16;
// a parked word is tried again this often; a quest's start and a cell's load
// try them on the next update
const PARKED_RETRY_MS = 2000;
const PARKED_MAX = 256;
// a parked word is let go after this long, as RefStateService's HOLD_MS: a
// stale fill for a quest that starts much later must not apply then (rung 4's
// fresh review, note 4)
const PARKED_HOLD_MS = 120_000;
const KNOWN_MAX = 4096;
const SUMMARY_MS = 30_000;
const LINES_PER_WINDOW = 5;

// thuum docs/DECISIONS.md ADR-068 rung 4 and amendment B (the state rows;
// docs/research/rung4-state-rows-2026-11-07.md (d)): AliasFill's ref half. An
// alias is nobody's (D-F1 C): every seat runs the quests, so its fills run on
// every seat, and whichever seat's scripts fill or clear an alias reports it;
// the server keeps the latest per quest and alias (an equal fill folded),
// relays it and serves the world's fills after a login's quest stages.
//
// The reporter. ScriptCallService hands over each ForceRefTo and Clear of the
// frame's batch (JS state alone); the last fill of each (quest, alias) in a
// frame wins and goes out on the update as AliasFill through the validator's
// bucket, unless it equals the last fill this seat reported or applied for
// the alias. A fill naming the player's own actor is never reported (ADR-068
// decision 6: the player's aliases are its seat's own; every seat fills its
// own player), nor one naming a server copy or a figure (no game-file id).
//
// The receiver. A relay or the login's serve is held by (quest, alias), the
// newest replacing a held one, and applied on the update through the
// engine's own natives: Quest.GetAlias, then ReferenceAlias.ForceRefTo or
// Clear, the platform's own calls (the seam tallies them "(platform)" and
// batches none, so nothing applied here is reported back). Rule 5: the quest
// must be a quest here, the alias a reference alias of it, the reference a
// reference here. A word is parked while its quest is not running here
// (QuestStageService's idiom: the engine's own start of the quest, the
// questStart event, brings it back) or its reference is not loaded, tried
// again every PARKED_RETRY_MS, and skipped when the alias already holds that
// reference.
//
// Lines for the lab (verb:quest-stage, the head the server's created half
// shares): "alias <quest>/<alias> reported: filled with <refr>" or "cleared"
// (the first few a window), "alias <quest>/<alias> filled with <refr>
// (served|relayed)" or "cleared (...)", and the 30 s summary "alias fills in
// 30 s: ...".
export class AliasFillService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("aliasFillMessage", (e) => this.onWord(e));
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.controller.on("update", () => this.onUpdate());
        this.controller.on("questStart", () => {
            this.parkedRetryAt = 0;
        });
        this.controller.on("cellFullyLoaded", () => {
            this.parkedRetryAt = 0;
        });
    }

    // ScriptCallService's: a ForceRefTo or Clear from the frame's batch (an
    // everyone row's call, its self the alias's quest). JS state alone (the
    // batch's context allows no game call)
    noteFill(call: ScriptCall): void {
        const alias = (call as AliasCall).alias;
        if (typeof alias !== "number" || !Number.isInteger(alias) || alias < 0 || alias > ALIAS_MAX) {
            this.noAlias++;
            return;
        }
        const quest = call.self;
        if (!quest || quest > LAST_GAME_FILE_ID) {
            this.unreportable++;
            return;
        }
        let refr = 0;
        if (call.fn === "ForceRefTo") {
            const arg = call.args[0];
            refr = arg !== null && typeof arg === "object" ? arg.formId : 0;
            if (!refr) {
                // ForceRefTo(None): the engine fills nothing
                this.unreportable++;
                return;
            }
        }
        if (refr === PLAYER_ID) {
            this.playersOwn++;
            return;
        }
        if (refr > LAST_GAME_FILE_ID) {
            this.unreportable++;
            return;
        }
        const key = AliasFillService.key(quest, alias);
        if (this.reports.has(key)) {
            this.folded++;
            this.reports.delete(key);
        } else if (this.reports.size >= REPORTS_MAX) {
            const oldest = this.reports.keys().next().value;
            if (oldest !== undefined) {
                this.reports.delete(oldest);
                this.reportsDropped++;
            }
        }
        this.reports.set(key, { quest, alias, refr, served: false, why: "" });
    }

    // a relay or the login's serve (a message handler: JS state alone). The
    // ranges the validator holds a report to are checked here, the forms at
    // the apply (rule 5)
    private onWord(e: ConnectionMessage<AliasFillMessage>): void {
        const m = e.message;
        const ok = typeof m.quest === "number" && Number.isInteger(m.quest) && m.quest > 0 && m.quest <= LAST_GAME_FILE_ID
            && typeof m.alias === "number" && Number.isInteger(m.alias) && m.alias >= 0 && m.alias <= ALIAS_MAX
            && typeof m.refr === "number" && Number.isInteger(m.refr) && m.refr >= 0 && m.refr <= LAST_GAME_FILE_ID;
        if (!ok) {
            logError(this, `alias fill from the server out of range, not held: quest ${String(m.quest)}, alias ${String(m.alias)}, refr ${String(m.refr)}`);
            return;
        }
        const key = AliasFillService.key(m.quest, m.alias);
        // the newest word for the alias replaces a held or parked one
        this.parked.delete(key);
        this.incoming.delete(key);
        this.incoming.set(key, { quest: m.quest, alias: m.alias, refr: m.refr, served: m.served === true, why: "" });
    }

    // a new connection (connectionAccepted, ahead of all its messages): the
    // server serves the world's fills anew; this seat's own unsent reports
    // stay (its engine's fills), the baseline of what was said goes
    private onConnectionAccepted(): void {
        this.incoming.clear();
        this.parked.clear();
        this.known.clear();
    }

    private onUpdate(): void {
        const now = Date.now();
        if (this.reports.size > 0) {
            this.report(now);
        }
        if (this.parked.size > 0 && now >= this.parkedRetryAt) {
            this.parkedRetryAt = now + PARKED_RETRY_MS;
            // at most APPLIES_PER_UPDATE retried an update; the rest go on the
            // next update (the fresh review, note 4)
            let retried = 0;
            for (const [key, fill] of Array.from(this.parked.entries())) {
                if (retried >= APPLIES_PER_UPDATE) {
                    this.parkedRetryAt = 0;
                    break;
                }
                this.parked.delete(key);
                if (fill.parkedAt !== undefined && now - fill.parkedAt >= PARKED_HOLD_MS) {
                    this.parkedDropped++;
                    continue;
                }
                retried++;
                this.applyOne(key, fill);
            }
        }
        let applies = 0;
        for (const [key, fill] of Array.from(this.incoming.entries())) {
            if (applies >= APPLIES_PER_UPDATE) {
                break;
            }
            applies++;
            this.incoming.delete(key);
            this.applyOne(key, fill);
        }
        this.summarize(now);
    }

    private report(now: number): void {
        this.tokens = Math.min(REPORT_BURST, this.tokens + ((now - this.refilledAt) / 1000) * REPORT_PER_S);
        this.refilledAt = now;
        for (const [key, r] of Array.from(this.reports.entries())) {
            if (this.known.get(key) === r.refr) {
                // what the server holds already, by this seat's last report or
                // its apply of the server's word
                this.reports.delete(key);
                this.folded++;
                continue;
            }
            if (this.tokens < 1) {
                // the rest wait for the bucket
                return;
            }
            this.tokens -= 1;
            this.reports.delete(key);
            const message: AliasFillMessage = { t: MsgType.AliasFill, quest: r.quest, alias: r.alias, refr: r.refr, served: false };
            this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
            this.setKnown(key, r.refr);
            this.sent++;
            if (this.lines < LINES_PER_WINDOW) {
                this.lines++;
                logLab(TAG, `alias ${AliasFillService.label(r.quest, r.alias)} reported: ${r.refr !== 0 ? `filled with ${r.refr.toString(16)}` : "cleared"}`);
            }
        }
    }

    // one word's apply in the update's context; parked when its quest is not
    // running here or its reference is not loaded
    private applyOne(key: string, fill: Fill): void {
        const name = AliasFillService.label(fill.quest, fill.alias);
        const how = fill.served ? "served" : "relayed";
        try {
            const quest = this.sp.Quest.from(this.sp.Game.getFormEx(fill.quest));
            if (!quest) {
                this.refused++;
                if (!this.missingQuests.has(fill.quest)) {
                    this.missingQuests.add(fill.quest);
                    logError(this, `alias ${name} (${how}): ${fill.quest.toString(16)} is not a quest in this game`);
                }
                return;
            }
            if (!quest.isRunning()) {
                this.park(key, fill, "its quest is not running here");
                return;
            }
            const ref = fill.refr !== 0 ? this.sp.ObjectReference.from(this.sp.Game.getFormEx(fill.refr)) : null;
            if (fill.refr !== 0 && !ref) {
                this.park(key, fill, "its reference is not loaded here");
                return;
            }
            const alias = this.sp.ReferenceAlias.from(quest.getAlias(fill.alias));
            if (!alias) {
                this.refused++;
                logError(this, `alias ${name} (${how}): no reference alias ${fill.alias} on quest ${fill.quest.toString(16)} here`);
                return;
            }
            const holds = alias.getReference();
            const holdsId = holds ? holds.getFormID() : 0;
            this.setKnown(key, fill.refr);
            if (holdsId === fill.refr) {
                this.hereAlready++;
                return;
            }
            if (ref) {
                alias.forceRefTo(ref);
            } else {
                alias.clear();
            }
            this.applied++;
            logLab(TAG, `alias ${name} ${fill.refr !== 0 ? `filled with ${fill.refr.toString(16)}` : "cleared"} (${how})${holdsId !== 0 ? `, it held ${holdsId.toString(16)}` : ""}`);
        } catch (e) {
            this.refused++;
            logError(this, `alias ${name} (${how}): the apply threw`, e);
        }
    }

    private park(key: string, fill: Fill, why: string): void {
        if (!this.parked.has(key) && this.parked.size >= PARKED_MAX) {
            const oldest = this.parked.keys().next().value;
            if (oldest !== undefined) {
                this.parked.delete(oldest);
                this.parkedDropped++;
            }
        }
        if (fill.why !== why) {
            this.parkings++;
        }
        fill.why = why;
        if (fill.parkedAt === undefined) {
            fill.parkedAt = Date.now();
        }
        this.parked.set(key, fill);
    }

    private setKnown(key: string, refr: number): void {
        if (this.known.size >= KNOWN_MAX && !this.known.has(key)) {
            this.known.clear();
        }
        this.known.set(key, refr);
    }

    // the window's counts, said once a window when any moved
    private summarize(now: number): void {
        if (now - this.lastSummary < SUMMARY_MS) {
            return;
        }
        this.lastSummary = now;
        this.lines = 0;
        const total = this.sent + this.folded + this.applied + this.hereAlready + this.parkings + this.refused
            + this.playersOwn + this.unreportable + this.noAlias + this.reportsDropped + this.parkedDropped;
        if (total > 0) {
            // short of printConsole's 128-byte cut (ScriptCallService's note)
            logLab(TAG, `alias fills in ${SUMMARY_MS / 1000} s: ${this.sent} reported, ${this.folded} folded, ${this.applied} applied, ${this.hereAlready} here already, ${this.parked.size} parked`);
            logTrace(this, `alias fills in ${SUMMARY_MS / 1000} s: ${this.refused} refused, ${this.playersOwn} the player's own, ${this.unreportable} unreportable, ${this.noAlias} with no alias id, ${this.parkings} parkings, ${this.reportsDropped + this.parkedDropped} dropped past the caps`);
            if (this.parked.size > 0) {
                const first = this.parked.values().next().value;
                if (first !== undefined) {
                    logTrace(this, `alias ${AliasFillService.label(first.quest, first.alias)} waits: ${first.why}`);
                }
            }
        }
        this.sent = 0;
        this.folded = 0;
        this.applied = 0;
        this.hereAlready = 0;
        this.parkings = 0;
        this.refused = 0;
        this.playersOwn = 0;
        this.unreportable = 0;
        this.noAlias = 0;
        this.reportsDropped = 0;
        this.parkedDropped = 0;
    }

    private static key(quest: number, alias: number): string {
        return `${quest}:${alias}`;
    }

    // the quest in hex and the alias's id, the lines' head
    private static label(quest: number, alias: number): string {
        return `${quest.toString(16)}/${alias}`;
    }

    // this seat's fills to report, by (quest, alias), the last of a frame
    private readonly reports = new Map<string, Fill>();
    // the server's words to apply, in their order, and the parked ones
    private readonly incoming = new Map<string, Fill>();
    private readonly parked = new Map<string, Fill>();
    // the last fill this seat reported or applied per (quest, alias): what the
    // server holds as far as this seat knows
    private readonly known = new Map<string, number>();
    private readonly missingQuests = new Set<number>();
    private parkedRetryAt = 0;
    private tokens = REPORT_BURST;
    private refilledAt = Date.now();
    private lastSummary = Date.now();
    private lines = 0;
    private sent = 0;
    private folded = 0;
    private applied = 0;
    private hereAlready = 0;
    private parkings = 0;
    private refused = 0;
    private playersOwn = 0;
    private unreportable = 0;
    private noAlias = 0;
    private reportsDropped = 0;
    private parkedDropped = 0;
}
