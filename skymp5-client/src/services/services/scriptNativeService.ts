import { ScriptNativeEvent } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// thuum docs/DECISIONS.md ADR-062 decision 2 (slice 1b; Eli: "trust should
// LARGELY be the default"): the consequences a hosted cell's scripts have on
// server-owned state, as the platform's hooks on the natives' entries raise
// them (ScriptNatives.cpp, the event scriptNative). Slice 1b logs and counts
// every call (the census of live calls, the trace seed for slice 2's rows:
// enabled state, placements, deletes, alias fills, destruction, world
// globals) and keeps the script kills' marks, so DeathReportService names a
// host-run script's kill (cause 3) and the server takes it as the host's
// word. Slice 2 turns the rows into reports.
export class ScriptNativeService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("scriptNative", (e) => this.onNative(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // a script's Kill on `localId` within the window, with the killer it
    // named (a form id, 0 for none); undefined otherwise
    scriptKill(localId: number): { at: number; killer: number } | undefined {
        const mark = this.kills.get(localId);
        if (!mark) {
            return undefined;
        }
        if (Date.now() - mark.at > ScriptNativeService.killWindowMs) {
            this.kills.delete(localId);
            return undefined;
        }
        return mark;
    }

    private onNative(e: ScriptNativeEvent) {
        this.counts.set(e.native, (this.counts.get(e.native) ?? 0) + 1);
        const arg = e.arg ? ` arg ${e.arg.toString(16)}` : "";
        const value = e.value ? ` value ${e.value}` : "";
        const cell = e.cell ? ` cell ${e.cell.toString(16)}` : "";
        logLab("verb:scripts", `native ${e.native} on ${e.self.toString(16)}${arg}${value}${cell}${e.actor ? " (actor)" : ""}`);
        if (e.native === "Actor.Kill" || e.native === "Actor.KillSilent") {
            if (this.kills.size >= 64) {
                this.kills.clear();
            }
            this.kills.set(e.self, { at: Date.now(), killer: e.arg });
        }
    }

    // the census, once a window: how many calls of each native this seat's
    // scripts made (the lab reads it; slice 2 sizes its rows by it)
    private onUpdate() {
        const now = Date.now();
        if (now - this.lastSummary < ScriptNativeService.summaryMs) {
            return;
        }
        this.lastSummary = now;
        if (this.counts.size === 0) {
            return;
        }
        const parts: string[] = [];
        let total = 0;
        this.counts.forEach((n, name) => {
            total += n;
            parts.push(`${name} ${n}`);
        });
        logLab("verb:scripts", `${total} native call(s) in ${ScriptNativeService.summaryMs / 1000} s: ${parts.sort().join(", ")}`);
        this.counts.clear();
        logTrace(this, `${this.kills.size} script kill mark(s) held`);
    }

    private readonly counts = new Map<string, number>();
    private readonly kills = new Map<number, { at: number; killer: number }>();
    private lastSummary = Date.now();
    private static readonly summaryMs = 30_000;
    private static readonly killWindowMs = 10_000;
}
