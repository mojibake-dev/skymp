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

// What PropService reads of a watched actor at an update: null when its local
// id names no remote id or no actor any more. figure: another player's figure
// (its knock is this seat's push's, the push-figure hook); puppet: an actor
// another seat simulates (HostService's word); hosted: an actor this seat's
// game runs, whatever its graph (a creature's queues no knock edge, since the
// action hook admits the master behaviour's graph alone)
export type ForcedRead = { figure: boolean; puppet: boolean; hosted: boolean; tracked: boolean; ragdoll: boolean };

// claim: another player's figure, claimed by touch 3; track: an actor this
// seat hosts, its body a row this seat streams as the host's
export type ForcedOutcome =
  | { localId: number; verdict: "claim" }
  | { localId: number; verdict: "track" }
  | { localId: number; verdict: "drop"; why: string };

// The watch itself, as PropService drives it: reach() from magicEffectApply,
// update() from every update. Nothing else gates it, since a shout that
// reaches only the other player's figure, or a creature this seat hosts,
// queues no knock and no bracketed impulse for any other step of the update
// to run on (the push's own impulse runs outside every bracket).
export class ForceWatch {
  // by local id: since when, and whether the server's relayed push (not this
  // seat's own spell) put it here
  private readonly watched = new Map<number, { since: number; relayed: boolean }>();

  get size(): number {
    return this.watched.size;
  }

  // a spell's effect applied: watched when this seat's own player (0x14) cast
  // it on another actor, from its first reach (a shout's effects each apply);
  // the update's read says whether it is a figure, one this seat hosts, or
  // neither
  reach(caster: number, target: number, now: number): boolean {
    if (caster !== 0x14 || target === 0x14 || this.watched.has(target)) {
      return false;
    }
    this.watched.set(target, { since: now, relayed: false });
    return true;
  }

  // the server's relayed push (another seat's push onto an actor this seat's
  // game runs, OnPushActor's PushActorAway snippet): watched for this seat to
  // track once its engine reads it knocked, never to claim, since the push is
  // another seat's
  relayed(target: number, now: number): boolean {
    if (target === 0 || target === 0x14 || this.watched.has(target)) {
      return false;
    }
    this.watched.set(target, { since: now, relayed: true });
    return true;
  }

  update(now: number, read: (localId: number) => ForcedRead | null): ForcedOutcome[] {
    const out: ForcedOutcome[] = [];
    this.watched.forEach(({ since, relayed }, localId) => {
      const r = read(localId);
      if (r === null || (!r.figure && !r.hosted) || (relayed && !r.hosted)) {
        this.watched.delete(localId);
        out.push({ localId, verdict: "drop", why: r === null ? "no actor"
          : relayed ? "a relayed push onto an actor this seat does not host" : "neither another player's figure nor this seat's" });
        return;
      }
      const verdict = forceWatchVerdict({ since, now, ragdoll: r.ragdoll, tracked: r.tracked });
      if (verdict === "wait") {
        return;
      }
      this.watched.delete(localId);
      if (verdict === "claim") {
        out.push({ localId, verdict: r.figure ? "claim" : "track" });
      } else {
        out.push({ localId, verdict, why: r.tracked ? "already a row" : "never knocked" });
      }
    });
    return out;
  }
}
