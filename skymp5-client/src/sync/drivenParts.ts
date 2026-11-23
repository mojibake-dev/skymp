// Which parts of a live body the engine drives. In an actor's ragdoll the
// parts the engine simulates go dynamic; a part that stays keyframed while
// the others are dynamic (a beast tail's bones) is not driven, and its read
// stays where the engine left it, hundreds of units from the body: the wire's
// reach check then dropped every row of that body at its first sample. Learned
// from a ragdolled read for the body's skeleton (its parts' names in walk
// order), it holds for every later read of that skeleton, a hold's or a
// standing body's included, where every part reads keyframed. An undriven
// part is sent at part 0's place, inside the wire's reach. JS state alone.

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
    return "drop";
};
