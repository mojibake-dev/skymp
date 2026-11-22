// The hit shaders this game plays on an actor for the server's set of its
// effects (MagicEffectsService's shader pass). JS state alone: the caller
// reads each effect's shader and whether this game's engine runs that effect
// on the actor, and plays and stops what this returns.
export interface ShaderEffect {
  // the effect's hit shader (MagicEffect.GetHitShader), 0 for none
  shader: number;
  // whether this game's engine runs the effect on the actor now
  // (Actor.HasMagicEffect)
  engineRuns: boolean;
  // the seconds it shows
  seconds: number;
}

export interface ShaderPass {
  play: Array<{ shader: number; seconds: number }>;
  stop: number[];
  // what this game shows for the set now, the next pass's `before`
  shown: Set<number>;
}

// Each effect's shader plays for its seconds; a shader shown before and no
// longer named is stopped.
export function shaderPass(effects: ShaderEffect[], before: Set<number>): ShaderPass {
  const play: Array<{ shader: number; seconds: number }> = [];
  const shown = new Set<number>();
  for (const effect of effects) {
    if (!effect.shader) {
      continue;
    }
    shown.add(effect.shader);
    play.push({ shader: effect.shader, seconds: effect.seconds });
  }
  const stop: number[] = [];
  before.forEach((shader) => {
    if (!shown.has(shader)) {
      stop.push(shader);
    }
  });
  return { play, stop, shown };
}
