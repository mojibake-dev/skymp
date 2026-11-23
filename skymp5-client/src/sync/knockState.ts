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

// thuum tracker #175 (Eli's second look: "the grip pulls a guy, but he's
// planking on ariadnes screen"; #178, a shout on a player): a copy this game
// follows as another seat's live body (the knocked actor's ragdoll streamed
// on the prop channel, PropService) is the stream's alone until its own
// knock ends: no movement sample moves or turns it, since a sample of a
// ragdolled actor carries the ragdoll's root and its tilt, which laid the
// standing copy flat. PropService marks it at the follow and clears it once
// the copy reads upright; forgotten after followedDownMs unrenewed
const followedDown = new Map<number, number>();
const followedDownMs = 2000;

export const markFollowedDown = (refrId: number): void => {
  followedDown.set(refrId, Date.now());
};

export const clearFollowedDown = (refrId: number): void => {
  followedDown.delete(refrId);
};

export const isFollowedDown = (refrId: number): boolean => {
  const at = followedDown.get(refrId);
  return at !== undefined && Date.now() - at <= followedDownMs;
};

// This game's own player's body under another player's force (a grip's hold
// or a throw's flight whose stream this game follows, PropService's handed
// own player): the forcing player's game is the body's only simulator, its
// stream the body's record, so this game sends no movement of its own player
// until the hand-back word. PropService marks it while a word names another
// player for the body and clears it at the hand-back; forgotten after
// ownHandedMs unrenewed, so a lost clear stops no movement for long
let ownHandedAt: number | undefined;
const ownHandedMs = 2000;

export const markOwnHanded = (now: number = Date.now()): void => {
  ownHandedAt = now;
};

export const clearOwnHanded = (): void => {
  ownHandedAt = undefined;
};

export const isOwnHanded = (now: number = Date.now()): boolean =>
  ownHandedAt !== undefined && now - ownHandedAt <= ownHandedMs;
