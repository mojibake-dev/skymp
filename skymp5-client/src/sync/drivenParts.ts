// Which parts of a live body the engine drives. In an actor's ragdoll the
// parts the engine simulates go dynamic; a part that stays keyframed while
// the others are dynamic (a beast tail's bones) is not driven, and its read
// stays where the engine left it, hundreds of units from the body: the wire's
// reach check then dropped every row of that body at its first sample. Learned
// from a ragdolled read for the body's skeleton (its parts' names in walk
// order), it holds for every later read of that skeleton, a hold's or a
// standing body's included, where every part reads keyframed. An undriven
// part is sent at part 0's place, inside the wire's reach. JS state alone.

import { Point, boneAnchor } from "./boneAnchor";

export interface WalkPart {
    node: string;
    motion: number;
}

// Havok's motion types as the walk reports them: dynamic 2 and 3, keyframed 4
const KEYFRAMED = 4;
const isDynamic = (motion: number) => motion === 2 || motion === 3;

export const skeletonKey = (parts: readonly WalkPart[]): string => parts.map((p) => p.node).join("|");

// the undriven parts' indices from a read, or undefined when the read says
// nothing (the actor not ragdolled now, or no part dynamic)
export const undrivenIn = (parts: readonly WalkPart[], ragdoll: boolean): number[] | undefined => {
    if (!ragdoll || !parts.some((p) => isDynamic(p.motion))) {
        return undefined;
    }
    const out: number[] = [];
    parts.forEach((p, i) => {
        if (p.motion === KEYFRAMED) {
            out.push(i);
        }
    });
    return out;
};

// each part's place as sent: an undriven part at part 0's, the rest as read
export const placeUndriven = (positions: readonly number[][], undriven: ReadonlySet<number>): number[][] => {
    const first = positions[0];
    return positions.map((p, i) => (undriven.has(i) && first !== undefined ? [first[0], first[1], first[2]] : [p[0], p[1], p[2]]));
};

// a body's sample with a part past the wire's reach, before its skeleton's
// undriven parts are known: a grip's first frames read every part keyframed,
// and the ragdoll shows its dynamic parts moments later, so the sample waits
// for that read, up to UNDRIVEN_WAIT_MS, rather than drop the row
export const UNDRIVEN_WAIT_MS = 1000;
export type PastReachStep = "wait" | "drop";
export const pastReachStep = (learned: boolean, waitedMs: number): PastReachStep => {
    return !learned && waitedMs < UNDRIVEN_WAIT_MS ? "wait" : "drop";
};

// the skeleton's undriven parts after a read: a part once read keyframed
// among dynamic parts stays undriven (a later read with every part dynamic,
// the impose's or a settle's, proves nothing about the engine's driver)
export const learnedUndriven = (known: ReadonlySet<number> | undefined, found: readonly number[]): Set<number> => {
    const out = new Set<number>(known !== undefined ? known : []);
    found.forEach((i) => out.add(i));
    return out;
};

// the undriven parts each skeleton has taught, and the skeleton each body read
// as at its last class read (PropService.bodyClass)
export class UndrivenSkeletons {
    private readonly bySkeleton = new Map<string, Set<number>>();
    private readonly skeletonOf = new Map<number, string>();

    // the body's skeleton's undriven parts; undefined while that skeleton has
    // taught none, or the body has had no class read
    of(localId: number): ReadonlySet<number> | undefined {
        return this.bySkeleton.get(this.skeletonOf.get(localId) ?? "");
    }

    // a class read of the body: its skeleton noted and what the read teaches
    // kept; the skeleton's undriven parts when they changed, else undefined
    learn(localId: number, parts: readonly WalkPart[], ragdoll: boolean): ReadonlySet<number> | undefined {
        const key = skeletonKey(parts);
        this.skeletonOf.set(localId, key);
        const found = undrivenIn(parts, ragdoll);
        if (found === undefined) {
            return undefined;
        }
        const known = this.bySkeleton.get(key);
        const next = new Set(found);
        if (known !== undefined && known.size === next.size && found.every((i) => known.has(i))) {
            return undefined;
        }
        this.bySkeleton.set(key, next);
        return next;
    }
}

// One sample of a live body of parts through the undriven rule, in the sample
// path's order (PropService.bonesOf): the skeleton's undriven parts placed at
// part 0's, then the wire's anchor (boneAnchor). A part past reach before the
// skeleton is learned reads the class now (learnNow) and places what it
// teaches, or waits for the ragdoll's read up to UNDRIVEN_WAIT_MS; a part
// still past reach drops the row. pastSince is when an unlearned part first
// lay past reach (0: not now), kept by the caller between samples
export interface PartsSample {
    root: Point;
    positions: readonly Point[];
    anchored: boolean;
    known: ReadonlySet<number> | undefined;
    learnNow: () => ReadonlySet<number> | undefined;
    pastSince: number;
    now: number;
    reach: number;
}

export type SampleStep =
    | { step: "send"; positions: Point[]; pos: Point; atPart: boolean; pastSince: number }
    | { step: "wait"; pastSince: number }
    | { step: "drop"; positions: Point[]; past: number; reach: number; pastSince: number };

const placed = (positions: readonly Point[], undriven: ReadonlySet<number> | undefined): Point[] =>
    undriven !== undefined && undriven.size > 0
        ? (placeUndriven(positions, undriven) as Point[])
        : positions.map((p) => [p[0], p[1], p[2]] as Point);

export const sampleStep = (s: PartsSample): SampleStep => {
    const positions = placed(s.positions, s.known);
    const anchor = boneAnchor(s.root, positions, s.reach, s.anchored);
    if ("past" in anchor) {
        return { step: "drop", positions, past: anchor.past, reach: anchor.reach, pastSince: s.pastSince };
    }
    return { step: "send", positions, pos: anchor.pos, atPart: anchor.atPart, pastSince: 0 };
};
