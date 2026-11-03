import { ScriptNativeEvent } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// thuum docs/DECISIONS.md ADR-062 decision 2 (slice 1b; Eli: "trust should
// LARGELY be the default"): the consequences a hosted cell's scripts have on
// server-owned state, as the platform's hooks on the natives' entries raise
// them (ScriptNatives.cpp, the event scriptNative). A live event is one call
// of a hooked native by a script frame on this seat for a world object in a
// cell this game hosts or an actor it simulates (the platform's own calls,
// the server's word applied, never count); a census event (self 0, count n)
// folds the quest, global and alias rows and the calls the native filtered
// out. Slice 1b logs the first few live calls per native per window and
// counts everything (the census, slice 2's seed) and keeps the script kills'
// marks, consumed by DeathReportService when it names cause 3. Slice 2 turns
// the rows into reports.
export class ScriptNativeService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("scriptNative", (e) => this.onNative(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // a script's Kill on `localId` within the window, with the killer it
    // named (a form id, 0 for none); undefined otherwise. The mark is
    // consumed: one death report per kill
    scriptKill(localId: number): { at: number; killer: number } | undefined {
        const mark = this.kills.get(localId);
        if (!mark) {
            return undefined;
        }
        this.kills.delete(localId);
        if (Date.now() - mark.at > ScriptNativeService.killWindowMs) {
            return undefined;
        }
        return mark;
    }

    private onNative(e: ScriptNativeEvent) {
        const n = e.count > 0 ? e.count : 1;
        this.counts.set(e.native, (this.counts.get(e.native) ?? 0) + n);
        if (e.self === 0) {
            return; // a census event: counted above
        }
        const logged = this.logged.get(e.native) ?? 0;
        if (logged < ScriptNativeService.logPerWindow) {
            this.logged.set(e.native, logged + 1);
            const arg = e.arg ? ` arg ${e.arg.toString(16)}` : "";
            const value = e.value ? ` value ${e.value}` : "";
            const cell = e.cell ? ` cell ${e.cell.toString(16)}` : "";
            logLab("verb:scripts", `native ${e.native} on ${e.self.toString(16)}${arg}${value}${cell}${e.actor ? " (actor)" : ""}`);
        }
        if (e.native === "Actor.Kill" || e.native === "Actor.KillSilent") {
            if (this.kills.size >= 64) {
                this.kills.clear();
            }
            this.kills.set(e.self, { at: Date.now(), killer: e.arg });
        }
    }

    // the census, once a window: how many calls of each native this seat's
    // scripts made, the filtered ones by their suffix (the lab reads it;
    // slice 2 sizes its rows by it)
    private onUpdate() {
        const now = Date.now();
        if (now - this.lastSummary < ScriptNativeService.summaryMs) {
            return;
        }
        this.lastSummary = now;
        this.logged.clear();
        if (this.counts.size === 0) {
            return;
        }
        // one row per line: the console log the lab reads cuts a line near
        // 100 characters (x-script-natives v3, run 20261103-062800: every
        // summary ended in "ObjectReference.Se..." and the rows past the cut,
        // the relayed stage's "(platform)" key among them, never reached it)
        const names = Array.from(this.counts.keys()).sort();
        let total = 0;
        for (const name of names) {
            const n = this.counts.get(name) ?? 0;
            total += n;
            logLab("verb:scripts", `census ${name} ${n} in ${ScriptNativeService.summaryMs / 1000} s`);
        }
        logLab("verb:scripts", `${total} native call(s) in ${ScriptNativeService.summaryMs / 1000} s: ${names.length} row(s), one census line each`);
        this.counts.clear();
        logTrace(this, `${this.kills.size} script kill mark(s) held`);
    }

    private readonly counts = new Map<string, number>();
    private readonly logged = new Map<string, number>();
    private readonly kills = new Map<number, { at: number; killer: number }>();
    private lastSummary = Date.now();
    private static readonly summaryMs = 30_000;
    private static readonly killWindowMs = 10_000;
    private static readonly logPerWindow = 5;
}
