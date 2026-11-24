// A player's own game's watch of the looks other seats ask of it: a script
// another seat runs for this player (a word wall's look test) asked whether
// this player sees a target, which only this game's engine can tell. Each ask
// runs the target's watch for LOOK_WATCH_MS; while it runs the target is read
// every LOOK_CHECK_MS, and a read is sent at the watch's first read, at each
// change and after each ask (so a seat whose answer was lost hears it again).
// JS state alone.

// the wire's LOOK_WATCH_MS (wire-schema skymp.rs)
export const LOOK_WATCH_MS = 10000;
// how often a watched look is read
export const LOOK_CHECK_MS = 250;

interface Watch {
  until: number;
  last: boolean | undefined;
  due: boolean;
}

export class LookWatches {
  private watches = new Map<number, Watch>();

  // an ask arrived: the target's watch runs LOOK_WATCH_MS from now, and its
  // next read is sent whatever it reads
  ask(target: number, now: number): void {
    const watch = this.watches.get(target);
    if (watch) {
      watch.until = now + LOOK_WATCH_MS;
      watch.due = true;
      return;
    }
    this.watches.set(target, { until: now + LOOK_WATCH_MS, last: undefined, due: true });
  }

  // the targets to read now; a watch past its end is dropped
  targets(now: number): number[] {
    const live: number[] = [];
    this.watches.forEach((watch, target) => {
      if (now >= watch.until) {
        this.watches.delete(target);
      } else {
        live.push(target);
      }
    });
    return live;
  }

  // a read of a watched target: whether it is sent (the first, a change, or
  // one an ask made due); false for a target no watch holds
  read(target: number, los: boolean): boolean {
    const watch = this.watches.get(target);
    if (!watch) {
      return false;
    }
    const send = watch.due || watch.last !== los;
    watch.last = los;
    watch.due = false;
    return send;
  }
}
