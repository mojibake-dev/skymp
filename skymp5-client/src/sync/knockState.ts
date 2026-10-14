// thuum docs/verbs/push.md (the flight): a figure this game itself knocked
// down, as the shouter's game does with the vanilla push on the target's
// figure (ADR-029 A1's caster half), flies here as its player flies in its own
// game. Until its get-up nothing else may move it: the relayed Ragdoll's
// zero-force push gave it a fresh knock in place (x-push-flight 20261014-215458
// and -220105), and the movement relay, paused only once that relayed Ragdoll
// came, still moved it toward its player's reported positions in between
// (-225505: it flew about 150 units, then snapped 1064 after the get-up).
// Marked when the figure's graph takes a Ragdoll of this game's own
// (sync/animation.ts), cleared at its GetUpBegin, forgotten after
// knockDownMs if no get-up is seen.
const knockedDown = new Map<number, number>();
const knockDownMs = 10000;

export const markKnockedDown = (refrId: number): void => {
  knockedDown.set(refrId, Date.now());
};

export const clearKnockedDown = (refrId: number): void => {
  knockedDown.delete(refrId);
};

export const isKnockedDown = (refrId: number): boolean => {
  const at = knockedDown.get(refrId);
  return at !== undefined && Date.now() - at <= knockDownMs;
};
