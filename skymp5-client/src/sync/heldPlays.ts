// An owner's relayed animation this seat's copy refused: a copy whose model
// was just swapped (a destroyed record's) can have no graph ready for the
// event yet. The play is held and tried again on each update while the copy's
// 3D is loaded, until the graph takes it or HELD_PLAY_MS pass; a later relay
// of the same reference supersedes it. JS state alone.

export const HELD_PLAY_MS = 5000;

export interface HeldPlay {
  id: number;     // the reference's local id
  remote: number; // its id as the relay named it
  event: string;
  since: number;
}

export class HeldPlays {
  private held = new Map<number, HeldPlay>();

  // a refused play: held from now, in place of any play held for the reference
  hold(id: number, remote: number, event: string, now: number): void {
    this.held.set(id, { id, remote, event, since: now });
  }

  // a later relay of the reference: its held play goes
  drop(id: number): void {
    this.held.delete(id);
  }

  has(id: number): boolean {
    return this.held.has(id);
  }

  // the plays to try now, and the ones past the bound, which go
  due(now: number): { retry: HeldPlay[]; expired: HeldPlay[] } {
    const retry: HeldPlay[] = [];
    const expired: HeldPlay[] = [];
    this.held.forEach((play, id) => {
      if (now - play.since >= HELD_PLAY_MS) {
        this.held.delete(id);
        expired.push(play);
      } else {
        retry.push(play);
      }
    });
    return { retry, expired };
  }

  // the graph took the reference's held play: it goes, with how long it was held
  taken(id: number, now: number): (HeldPlay & { heldMs: number }) | undefined {
    const play = this.held.get(id);
    if (play === undefined) {
      return undefined;
    }
    this.held.delete(id);
    return { ...play, heldMs: now - play.since };
  }

  get size(): number {
    return this.held.size;
  }
}
