import { NiPoint3 } from "./movement";

// thuum docs/verbs/prop-physics.md (ADR-071 decision 3, physics rung 2): one
// stream of a prop another seat simulates, as the follower renders it. A pure
// module: no engine call and no clock read (every time is passed in), so a
// test runner can take it whole; PropService does the engine's part. The copy
// shows the owner's body a fixed delay behind the OWNER's own clock (each
// sample's tMs) and never ahead of it: snapshot interpolation (accessible:
// Fiedler, "Snapshot Interpolation", gafferongames.com, 2014; foundational:
// Bernier, "Latency Compensating Methods in Client/Server In-game Protocol
// Design and Optimization", GDC 2001). The render clock is this seat's clock
// less the stream's offset and the delay; the offset is the least one-way gap
// (arrival less tMs) over the last FOLLOW_OFFSET_SAMPLES samples, the sample
// least delayed by queues, NTP's clock filter's choice (accessible: Mills,
// "The NTP Clock Filter Algorithm", www.eecis.udel.edu/~mills/ntp/html/
// filter.html; foundational: Mills, "Internet time synchronization: the
// Network Time Protocol", IEEE Trans. Communications 39(10), 1991). Arrival
// times feed the offset alone; no segment is timed by them. Past the newest
// sample the copy holds there and the frame is counted dry (ADR-058 decision
// 3, "Nothing predicts"); an asleep sample, the stream's rest, applies when the
// render clock reaches it, never at its arrival.

// One relayed sample as the follower keeps it: PropTransform v2's fields (tMs
// the owner's clock at its read, lin game units a second, ang radians a
// second)
export type FollowSample = {
    epoch: number;
    seq: number;
    tMs: number;
    pos: NiPoint3;
    rot: NiPoint3;
    lin: NiPoint3;
    ang: NiPoint3;
    asleep: boolean;
    held: boolean;
};

// The seat a sample came from, as the word this seat holds at the sample's
// epoch names it: a player, 0 for the cell's host (a player-0 word), undefined
// while that word has not come (the unreliable sample overtook the reliable
// word, WIRE.md's two channels)
export type FollowSeat = number | undefined;

// One update of the render clock: the sample the copy goes to (the end of the
// pair the render clock is in; the newest once it is past them all), whether
// the copy was not sent toward it yet (one translate a segment), what is left
// of that segment on the owner's clock (0 or less once the render clock passed
// it), whether this update is a dry frame (the render clock past the newest
// awake sample: the copy holds there), and whether the render clock reached
// the stream's asleep sample at this update (the rest applies now, once)
export type FollowStep = {
    target: FollowSample;
    fresh: boolean;
    remainingMs: number;
    dry: boolean;
    rested: boolean;
};

// The samples a stream keeps, and the arrivals its offset is the least of
export const FOLLOW_SAMPLES_MAX = 8;
export const FOLLOW_OFFSET_SAMPLES = 16;

// Whether sequence a is newer than b within one epoch: serial number
// arithmetic on the u16, the server rule's own (wire-rules props.rs newer:
// a.wrapping_sub(b) in 1 to 32767; accessible: Fiedler, "Reliable Ordered
// Messages", gafferongames.com, 2016; foundational: Elz and Bush, "Serial
// Number Arithmetic", RFC 1982, 1996)
export const seqNewer = (a: number, b: number): boolean => {
    const d = (((a - b) % 65536) + 65536) % 65536;
    return d >= 1 && d <= 32767;
};

// whether sample a comes before b in a stream: by epoch, then by sequence
const before = (a: FollowSample, b: FollowSample): boolean =>
    a.epoch < b.epoch || (a.epoch === b.epoch && seqNewer(b.seq, a.seq));

// the sequence numbers from first to newest, both counted; a run across the
// wrap skips 0, which no sample carries (65535 wraps to 1)
const span = (run: { first: number; newest: number }): number =>
    ((((run.newest - run.first) % 65536) + 65536) % 65536) + 1 - (run.newest < run.first ? 1 : 0);

export class PropFollow {
    constructor(first: FollowSample, seat: FollowSeat, arrivalMs: number) {
        this.seat = seat;
        this.firstEpoch = first.epoch;
        this.run = { epoch: first.epoch, first: first.seq, newest: first.seq, received: 0 };
        this.take(first, seat, arrivalMs);
    }

    // Whether a sample is this stream's ("this"), a later one's ("next": a new
    // owner's clock, so the buffer and its offset start over), or older than
    // the stream ("stale": dropped, feeding nothing). One owner an epoch, so a
    // sample at an epoch the stream spans is its own; a later epoch is a new
    // stream (a grant, the host's wake after a rest), but for A7's wake: the
    // host's Woken sample streams under the player-0 word at epoch E and the
    // word naming the host by id follows at E + 1, the same seat and clock
    // (ADR-071 decision 2's A7; the rung 2 plan's section 5, "an epoch rise
    // with the same seat keeps them")
    classify(s: FollowSample, seat: FollowSeat): "this" | "next" | "stale" {
        if (s.epoch < this.firstEpoch) {
            return "stale";
        }
        const newest = this.newest();
        if (s.epoch <= newest.epoch) {
            return "this";
        }
        const wake = !newest.asleep && this.seat === 0 && seat !== 0 && s.epoch === newest.epoch + 1;
        return wake ? "this" : "next";
    }

    // A sample of this stream (classify said "this") at its arrival on this
    // seat's clock: its gap to the owner's clock joins the offset's window, and
    // it enters the buffer in order unless it repeats one there or the render
    // clock already reached it (late, counted as arrived). Returns whether it
    // entered
    take(s: FollowSample, seat: FollowSeat, arrivalMs: number): boolean {
        const buf = this.buffer;
        for (const b of buf) {
            if (b.epoch === s.epoch && b.seq === s.seq) {
                return false;
            }
        }
        this.offsets.push(arrivalMs - s.tMs);
        if (this.offsets.length > FOLLOW_OFFSET_SAMPLES) {
            this.offsets.shift();
        }
        let least = this.offsets[0];
        for (const o of this.offsets) {
            least = Math.min(least, o);
        }
        this.offsetMs = least;
        this.samples += 1;
        this.count(s);
        const newest = buf.length > 0 ? buf[buf.length - 1] : undefined;
        if (newest === undefined || before(newest, s)) {
            // an epoch rise kept (A7) names the seat its word names, or none
            // yet; at one epoch a seat heard late fills in
            if (newest !== undefined && s.epoch > newest.epoch) {
                this.seat = seat;
            } else if (this.seat === undefined) {
                this.seat = seat;
            }
            buf.push(s);
            if (buf.length > FOLLOW_SAMPLES_MAX) {
                buf.shift();
            }
            return true;
        }
        // reordered in flight: kept while the render clock has not reached it
        if (this.target !== undefined && !before(this.target, s)) {
            return false;
        }
        let i = buf.length - 1;
        while (i > 0 && before(s, buf[i - 1])) {
            --i;
        }
        buf.splice(i, 0, s);
        return true;
    }

    // The render clock at nowMs (this seat's clock, the arrivals' own) and the
    // update's step on it; the samples behind the pair it is in are dropped
    step(nowMs: number, delayMs: number): FollowStep {
        const render = nowMs - this.offsetMs - delayMs;
        const buf = this.buffer;
        const newest = buf[buf.length - 1];
        let i = 0;
        while (i < buf.length && buf[i].tMs <= render) {
            ++i;
        }
        const passed = i >= buf.length;
        const target = passed ? newest : buf[i];
        const fresh = this.target === undefined || this.target.epoch !== target.epoch || this.target.seq !== target.seq;
        this.target = target;
        const dry = passed && !newest.asleep;
        if (dry) {
            this.dry += 1;
        }
        const rested = passed && newest.asleep && !this.restApplied;
        if (rested) {
            this.restApplied = true;
        }
        const keepFrom = passed ? buf.length - 1 : Math.max(0, i - 1);
        if (keepFrom > 0) {
            buf.splice(0, keepFrom);
        }
        return { target, fresh, remainingMs: target.tMs - render, dry, rested };
    }

    // Whether the stream is live on the render clock: its newest sample awake
    // and the render clock less than quietMs past it (the owner's heartbeat
    // keeps an awake body's stream within that)
    live(nowMs: number, delayMs: number, quietMs: number): boolean {
        const newest = this.newest();
        return !newest.asleep && nowMs - this.offsetMs - delayMs - newest.tMs < quietMs;
    }

    // whether the stream ended at an asleep sample the render clock has not
    // reached yet
    restPending(): boolean {
        return this.newest().asleep && !this.restApplied;
    }

    newest(): FollowSample {
        return this.buffer[this.buffer.length - 1];
    }

    epoch(): number {
        return this.newest().epoch;
    }

    // a translate the copy took toward a target: the largest kept, a jump
    // counted
    noteStep(units: number, jumped: boolean): void {
        this.largestStep = Math.max(this.largestStep, units);
        if (jumped) {
            this.jumps += 1;
        }
    }

    // the sequence numbers of the stream's epochs that never arrived, each
    // epoch counted from the first one seen (a follower that joins mid-stream
    // counts from there); a late arrival is no gap
    gaps(): number {
        return Math.max(0, this.closedGaps + span(this.run) - this.run.received);
    }

    // the gap count's run of the newest epoch, and the closed runs' gaps
    private count(s: FollowSample) {
        const run = this.run;
        if (s.epoch > run.epoch) {
            this.closedGaps += span(run) - run.received;
            this.run = { epoch: s.epoch, first: s.seq, newest: s.seq, received: 1 };
            return;
        }
        if (s.epoch < run.epoch) {
            // a straggler of the epoch before (A7's kept stream): one of its
            // gaps filled
            this.closedGaps -= 1;
            return;
        }
        run.received += 1;
        if (seqNewer(s.seq, run.newest)) {
            run.newest = s.seq;
        } else if (seqNewer(run.first, s.seq)) {
            run.first = s.seq;
        }
    }

    // the stream's seat at its newest epoch (FollowSeat), and its figures for
    // the end line: samples arrived (each once), dry frames, jumps and the
    // largest translate the copy took, game units
    seat: FollowSeat;
    samples = 0;
    dry = 0;
    jumps = 0;
    largestStep = 0;
    // the samples by epoch then sequence, from the pair the render clock is in
    private readonly buffer: FollowSample[] = [];
    private readonly firstEpoch: number;
    private readonly offsets: number[] = [];
    private offsetMs = 0;
    // the sample the copy was last sent toward
    private target: FollowSample | undefined;
    private restApplied = false;
    private run: { epoch: number; first: number; newest: number; received: number };
    private closedGaps = 0;
}
