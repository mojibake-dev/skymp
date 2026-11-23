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

// What PropService reads of a watched figure at an update: null when its local
// id names no remote id or no actor of a figure's base any more
export type ForcedRead = { figure: boolean; tracked: boolean; ragdoll: boolean };

export type ForcedOutcome = { localId: number; verdict: "claim" } | { localId: number; verdict: "drop"; why: string };

// The watch itself, as PropService drives it: reach() from magicEffectApply,
// update() from every update. Nothing else gates it, since a shout that
// reaches only the other player's figure queues no knock and no impulse for
// any other step of the update to run on.
export class ForceWatch {
  private readonly watched = new Map<number, number>();

  get size(): number {
    return this.watched.size;
  }

  // a spell's effect applied: watched when this seat's own player (0x14) cast
  // it on a server-made figure (a local id at 0xff000000 or past), from its
  // first reach (a shout's effects each apply)
  reach(caster: number, target: number, now: number): boolean {
    if (caster !== 0x14 || target < 0xff000000 || this.watched.has(target)) {
      return false;
    }
    this.watched.set(target, now);
    return true;
  }

  update(now: number, read: (localId: number) => ForcedRead | null): ForcedOutcome[] {
    const out: ForcedOutcome[] = [];
    this.watched.forEach((since, localId) => {
      const r = read(localId);
      if (r === null || !r.figure) {
        this.watched.delete(localId);
        out.push({ localId, verdict: "drop", why: "no figure" });
        return;
      }
      const verdict = forceWatchVerdict({ since, now, ragdoll: r.ragdoll, tracked: r.tracked });
      if (verdict === "wait") {
        return;
      }
      this.watched.delete(localId);
      out.push(verdict === "claim" ? { localId, verdict } : { localId, verdict, why: r.tracked ? "already a row" : "never knocked" });
    });
    return out;
  }
}
