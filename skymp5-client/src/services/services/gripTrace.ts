import { logLab } from "../../logging";

// A read, never merged: the grip spell and its cloak's damage spell followed
// from the engine's spellCast, hit and magicEffectApply events to the native
// send, one line at every branch and early return on that path.
const watched = new Set<number>([0x1D005900, 0x0200E7DA]);
const counts = new Map<string, number>();

export function isWatchedSpell(id: number | null | undefined): boolean {
    return typeof id === "number" && watched.has(id);
}

// The first line of each kind, then one at each power of two of its count, so
// a stream's ticks stay a few lines and the counts still read.
export function gripTrace(kind: string, detail: string): void {
    const n = (counts.get(kind) || 0) + 1;
    counts.set(kind, n);
    if ((n & (n - 1)) === 0) {
        logLab("grip-trace", `${kind} (x${n}): ${detail}`);
    }
}

export function hex(id: number | null | undefined): string {
    return typeof id === "number" ? id.toString(16) : String(id);
}
