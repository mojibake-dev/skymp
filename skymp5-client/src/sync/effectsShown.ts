// What this game draws of the server's running effects on one actor: each
// effect's hit shader and hit effect art, for the seconds the server says are
// left, unless the engine here runs an effect of the same record on that
// actor and draws them itself. A second draw of one record's look stacks a
// second shader or model over the engine's: an attacker's own engine adds its
// enchanted hit to the actor it struck, and the server's record of the same
// hit reaches that game too. A shader or art two effects share shows for the
// longer; a negative seconds (no end, until the server's set drops it) is the
// longest.

export interface ServerEffect {
    // the effect's hit shader and hit effect art, 0 for none
    shader: number;
    art: number;
    // seconds left as this game shows them: positive, or negative for no end
    seconds: number;
    // the engine here runs an effect of this record on the actor
    engineRuns: boolean;
}

export interface EffectsShown {
    // form id to seconds, for each shader and art this game draws
    shaders: Map<number, number>;
    arts: Map<number, number>;
    // the shaders of the effects the engine here runs on the actor
    engineShaders: Set<number>;
}

function longer(a: number | undefined, b: number): number {
    if (a === undefined) {
        return b;
    }
    return a < 0 || b < 0 ? -1 : Math.max(a, b);
}

export function effectsShown(effects: ServerEffect[]): EffectsShown {
    const shaders = new Map<number, number>();
    const arts = new Map<number, number>();
    const engineShaders = new Set<number>();
    for (const e of effects) {
        if (e.engineRuns) {
            if (e.shader) {
                engineShaders.add(e.shader);
            }
            continue;
        }
        if (e.shader) {
            shaders.set(e.shader, longer(shaders.get(e.shader), e.seconds));
        }
        if (e.art) {
            arts.set(e.art, longer(arts.get(e.art), e.seconds));
        }
    }
    return { shaders, arts, engineShaders };
}

// the shaders this game drew before and draws no more, to stop; one whose
// effect the engine here now runs is left to run out its seconds, since a
// stop on the actor could end the engine's own draw of it as well
export function noLongerShown(before: Set<number>, now: Map<number, number>, engineShaders: Set<number>): number[] {
    const out: number[] = [];
    before.forEach((id) => {
        if (!now.has(id) && !engineShaders.has(id)) {
            out.push(id);
        }
    });
    return out;
}
