// The per-frame work's bounds: each engine call the client makes every frame
// is a native call, so the frame's cost grows with what it touches. These
// decide, from JS state alone, which calls a frame makes.

// The hosted actors whose movement is sampled this frame: each due once its
// last sample is older than its own rate (one never sampled is due at
// once), the longest waiting first, at most `budget` a frame. A slow frame
// samples no more actors than a fast one, so the sampling no longer slows
// the frame it is in.
export const dueSamples = (
  ids: ReadonlyArray<number>,
  lastSent: (id: number) => number | undefined,
  now: number,
  rateMs: (id: number) => number,
  budget: number,
): number[] => {
  const due: { id: number; waited: number }[] = [];
  for (const id of ids) {
    const last = lastSent(id);
    if (last === undefined) {
      due.push({ id, waited: Infinity });
    } else if (now - last > rateMs(id)) {
      due.push({ id, waited: now - last });
    }
  }
  due.sort((a, b) => b.waited - a.waited);
  return due.slice(0, Math.max(0, Math.floor(budget))).map((d) => d.id);
};

// Whether a view's movement is applied this frame: a new movement near the
// player at once, one farther away no sooner than `farRateMs` after the last
// apply (the newest movement then, so a far actor's moves land at that rate),
// and any view again `reassertMs` after its last apply.
export const movementApplyDue = (input: {
  changed: boolean;
  distance: number;
  nearUnits: number;
  sinceLastApplyMs: number;
  farRateMs: number;
  reassertMs: number;
}): boolean => {
  if (input.sinceLastApplyMs > input.reassertMs) {
    return true;
  }
  if (!input.changed) {
    return false;
  }
  return !(input.distance > input.nearUnits) || input.sinceLastApplyMs >= input.farRateMs;
};

// A hosted actor's sample rate: the send rate while another player is within
// `nearUnits` of its last sample or it was in combat then (the server checks
// its hits against the record), the far rate otherwise, since no other game
// draws it near and the server's record only has to stay roughly current
export const hostedSampleRateMs = (input: {
  last: { pos: ReadonlyArray<number>; worldOrCell: number; inCombat: boolean } | undefined;
  others: ReadonlyArray<{ pos: ReadonlyArray<number>; worldOrCell: number }>;
  nearUnits: number;
  sendRateMs: number;
  farRateMs: number;
}): number => {
  const last = input.last;
  if (!last || last.inCombat) {
    return input.sendRateMs;
  }
  for (const other of input.others) {
    if (other.worldOrCell === last.worldOrCell && !(distanceBetween(last.pos, other.pos) > input.nearUnits)) {
      return input.sendRateMs;
    }
  }
  return Math.max(input.sendRateMs, input.farRateMs);
};

// The distance in the x-y plane and height from `a` to `b`; Infinity when
// either is unknown, so an unknown distance never reads as near.
export const distanceBetween = (a: ReadonlyArray<number> | undefined, b: ReadonlyArray<number> | undefined): number => {
  if (!a || !b || a.length < 3 || b.length < 3) {
    return Infinity;
  }
  const dx = a[0] - b[0];
  const dy = a[1] - b[1];
  const dz = a[2] - b[2];
  return Math.sqrt(dx * dx + dy * dy + dz * dz);
};

// One value read from the engine and kept for `ttlMs`: a cell a reference
// stands in changes far less often than every frame.
export class Timed<V> {
  private value: V | undefined;
  private at = -Infinity;

  get(now: number, ttlMs: number, read: () => V): V {
    if (this.value === undefined || now - this.at >= ttlMs) {
      this.value = read();
      this.at = now;
    }
    return this.value;
  }
}

// One engine write's last value: a write is made when its value differs from
// the last one made, or `reassertMs` after it (the engine may have moved off
// the value by itself), and skipped otherwise.
export class LastWritten<V> {
  private value: V | undefined;
  private at = -Infinity;
  private written = false;

  due(value: V, now: number, reassertMs: number, same: (a: V, b: V) => boolean = (a, b) => a === b): boolean {
    if (this.written && this.value !== undefined && same(this.value, value) && now - this.at < reassertMs) {
      return false;
    }
    this.value = value;
    this.at = now;
    this.written = true;
    return true;
  }
}

// A sweep over a set of ids that reads each of them once every `passMs`, a
// share of them a frame: a pass is the set as it stood when the pass began,
// each frame's share its part of the pass for the time since the frame
// before, at most `maxShare`, so no frame pays for the whole set.
export class RefSweep {
  private pass: number[] = [];
  private at = 0;
  private last: number | undefined = undefined;

  constructor(private readonly passMs: number, private readonly maxShare: number) {}

  // the ids this frame reads; `ids` is asked for the set only when a pass
  // begins, so an id added during a pass is read in the next one, and the
  // caller skips an id gone since its pass began
  next(ids: () => Iterable<number>, now: number): number[] {
    if (this.at >= this.pass.length) {
      this.pass = Array.from(ids());
      this.at = 0;
    }
    // the first frame has no frame before it: one id, as any frame's least share
    const since = this.last === undefined ? 0 : Math.max(0, now - this.last);
    this.last = now;
    const share = Math.min(this.maxShare, Math.max(1, Math.ceil(this.pass.length * since / this.passMs)));
    const slice = this.pass.slice(this.at, this.at + share);
    this.at += slice.length;
    return slice;
  }
}

// Every pass timer's line comes every PASS_SAY_MS; a run of PASS_LONG_MS or
// more, half a frame at 60 fps, is counted as long.
export const PASS_SAY_MS = 10000;
export const PASS_LONG_MS = 8;

// The clock a pass is timed on, ms: performance.now() where the platform's
// Node exposes it (monotonic, sub-millisecond), else Date.now().
const perf = (globalThis as unknown as { performance?: { now?: () => number } }).performance;
export const passClockMs = (): number => (perf !== undefined && typeof perf.now === "function" ? perf.now() : Date.now());

// The time a pass takes, said as one line every `sayMs` so a frame capture's
// long frames can be laid against it: its runs since the last line, the
// pieces a run on average (a rescan's views, a frame's movement applies),
// the runs' mean and longest time, and how many took `longMs` or more. `add`
// counts a piece's time toward the frame's run; `close`, once a frame, ends
// that run and returns the line when one is due.
export class PassTimes {
  private runs = 0;
  private pieces = 0;
  private totalMs = 0;
  private longestMs = 0;
  private longRuns = 0;
  private frameOpen = false;
  private frameMs = 0;
  private framePieces = 0;
  private since: number | undefined = undefined;

  constructor(readonly name: string, private readonly sayMs: number, private readonly longMs: number) {}

  add(ms: number, pieces = 1): void {
    this.frameOpen = true;
    this.frameMs += Math.max(0, ms);
    this.framePieces += pieces;
  }

  close(now: number): string | undefined {
    if (this.frameOpen) {
      this.runs += 1;
      this.pieces += this.framePieces;
      this.totalMs += this.frameMs;
      this.longestMs = Math.max(this.longestMs, this.frameMs);
      if (this.frameMs >= this.longMs) {
        this.longRuns += 1;
      }
      this.frameOpen = false;
      this.frameMs = 0;
      this.framePieces = 0;
    }
    if (this.since === undefined) {
      this.since = now;
      return undefined;
    }
    if (now - this.since < this.sayMs) {
      return undefined;
    }
    const span = `${this.name}: ${this.runs} runs in ${((now - this.since) / 1000).toFixed(1)} s`;
    const line = this.runs === 0 ? span
      : `${span}, ${(this.pieces / this.runs).toFixed(1)} pieces a run, mean ${(this.totalMs / this.runs).toFixed(1)} ms, ` +
        `longest ${this.longestMs.toFixed(1)} ms, ${this.longRuns} at ${this.longMs} ms or more`;
    this.runs = 0;
    this.pieces = 0;
    this.totalMs = 0;
    this.longestMs = 0;
    this.longRuns = 0;
    this.since = now;
    return line;
  }
}
