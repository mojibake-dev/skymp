// A figure this seat's own player's spell just reached (magicEffectApply,
// PropService): its knock can only be this seat's push, since the push-figure
// hook runs a push onto another player's figure on its pusher's game alone,
// and neither the impulse report (the push's knock is no bracketed impulse)
// nor the knock edge (the action hook passes the player and hosted actors
// only) names it. So the figure is watched for WATCH_MS: its engine reading
// it ragdolled makes it a row this seat claims by touch 3, which the server
// grants on the hit it took; one already tracked (followed, gripped, owned)
// or never knocked within the window is dropped from the watch.
export const WATCH_MS = 2000;

export type WatchVerdict = "claim" | "drop" | "wait";

export const forceWatchVerdict = (f: { since: number; now: number; ragdoll: boolean; tracked: boolean }): WatchVerdict => {
  if (f.tracked) {
    return "drop";
  }
  if (f.ragdoll) {
    return "claim";
  }
  return f.now - f.since > WATCH_MS ? "drop" : "wait";
};
