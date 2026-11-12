//! Physics props (thuum docs/verbs/prop-physics.md, ADR-058; the arbitration
//! rule A1 to A7 of ADR-063's props note, accepted by ADR-071 decision 2):
//! one simulating seat per prop, Garry's Mod's shape without its server. A
//! prop (a kicked basket, a dropped sword, a trap's falling rock) is
//! simulated by exactly one seat at a time: the cell's host by default, the
//! seat whose engine set it moving while it moves, a grabber while its
//! engine holds it. The server decides who simulates (the claim, R0) and
//! records the simulator's transforms (R2: only the owner's are taken).
//! Ownership changes by the server's word alone (`PropOwner`): a grant, the
//! return to the host at the owner's own asleep sample (player 0, the host
//! by default), the host's own wake of a resting prop (by id), a departure,
//! an unload. No timer hands a prop back (playtest 41, Eli: "item physics
//! sync is more broken than it was when we STARTED testing": the 3 s lease
//! and the 3 s silence returned props the owner still streamed, and no seat
//! heard it), and the grab's cap is not built either (Eli's D-PH3 A,
//! 2026-11-11, ADR-071's amendment: a hold ends by the holder's release, its
//! rest or its departure), so who owns a prop is a function of the words
//! alone and takes no clock; the moving window judges a claim and the step
//! allowance a sample, and neither ever returns a row. The claim's rows: a
//! grab beats a touch and nothing beats a grab (Held); a contact, a drop or
//! an impulse never takes a prop another seat streams awake (Moving), a
//! trap's world body included (thuum D-S3's first firing wins by the same
//! row, ADR-071 decision 8); anything at rest or stalled is the newest
//! touch's. The owner table is ADR-050 decision 4's second row kind, "prop",
//! beside the cells rule's follows, margins and claims (`cells.rs`); the cell
//! is still the policy: a resting prop in a cell nobody hosts freezes at the
//! record. The core gathers the facts (who hosts the prop's cell, where the
//! record stands, the wall clock) and asks here; nothing here knows a
//! position. A pickup (an item's activation, which gives the item and
//! deletes a server-made reference for every seat) is a take, read off the
//! claim's rows for a touch that is no grab, and changes no row (`pickup`).
//!
//! A sample is judged by its epoch first and then by the owner table, never
//! by its position (ADR-066 decision 4; ADR-071 decision 6, physics rung 2):
//! one below the prop's epoch is a sample of an earlier tenure (Stale), one
//! above it names a word the server never said (Future); a sequence not
//! newer than the row's last at the epoch is an unreliable sample overtaken
//! in flight (Late); the step budget refills by the owner's own clock,
//! bounded by the server's, and a tenure's first sample may spend half a
//! second of that clock's time at once (physics rung 3a: a blasted body
//! flies before its mover hears the word); a rest past the budget is clamped
//! and taken (Clamped), so every seat ends at one record; and the engine's
//! own grab rides each sample (`held`, Eli's D-PH3 A on the wire).

use std::collections::{HashMap, HashSet};

/// How recent the owner's last accepted sample (or its grant) must be for the
/// prop to count as MOVING, which a contact, a drop or an impulse from another
/// seat cannot take (A3: a non-owner's contact never takes a moving prop). An
/// owner streams at most every 100 ms while the body moves, at least every
/// 500 ms while it is awake or held (the client's heartbeat, ADR-071's D-PH4
/// B, sent at the first frame past 500 ms) and one asleep sample at rest. The
/// window is about three heartbeat intervals and a frame, so two lost
/// unreliable beats in a row still read as moving (one lost beat alone left a
/// gap of 1,000 to 1,066 ms, past a 1,000 ms window: the review of
/// b7151a42), the way a failure detector sets its timeout at several
/// heartbeat intervals (accessible: Joshi, "HeartBeat", Patterns of
/// Distributed Systems; foundational: Ongaro and Ousterhout, "In Search of an
/// Understandable Consensus Algorithm", USENIX ATC 2014, section 5.6). A row
/// quieter than this is stalled (a stream that died without its asleep
/// sample) or its samples are not getting through; either is the next
/// touch's. A judgment of a claim, never a return: no clock changes a row's
/// owner. The same length as the client's STREAM_LIVE_MS. UNMEASURED (thuum
/// docs/engine/havok-props.md E10 sizes it).
pub const PROP_MOVING_MS: u64 = 1_600;

/// The step budget's pace: the rate a row's distance allowance refills at
/// (`PropRow::allowance`, a token bucket on distance). A sample farther from
/// the record than the allowance holds is refused and leaves the record
/// where it stood; an accepted one spends the units it moved the record. So
/// a seat cannot carry a prop across the world in one message, and over any
/// window no owner moves the record faster than a kicked body flies, at any
/// sample rate. UNMEASURED: x-prop-push sizes it.
pub const PROP_MAX_SPEED_UNITS_PER_S: f32 = 6_000.0;
/// The most a row's allowance holds for a sample after an epoch's first:
/// one honest 100 ms step at `PROP_MAX_SPEED_UNITS_PER_S` (the client's
/// cadence), and what an accepted sample leaves it, whatever the sample's
/// own allowance held. It replaces a per-sample floor of 128 units, which
/// the review of 9ff1895e (finding 3) found was the limit at the prop
/// budget's 96 samples a second: 12,288 units a second, twice the cap.
/// UNMEASURED.
pub const PROP_STEP_ALLOWANCE_UNITS: f32 = PROP_MAX_SPEED_UNITS_PER_S / 10.0;

/// The most an epoch's first sample's allowance holds (physics rung 3a):
/// half a second of flight at `PROP_MAX_SPEED_UNITS_PER_S`, the number of
/// 020de9ab's first-sample budget (playtest 41's cabbages), rebuilt on the
/// step budget's one clock. A body an impulse moves flies before the word
/// naming its mover reaches that seat (the impact, the claim, the grant, the
/// word's way back, the seat's next read; a lost reliable packet waits for
/// renet's 300 ms resend), and a host's far body its own engine woke waits
/// for the next rescan, so the tenure's first sample stands farther from the
/// record than one step: the cart's cabbage 0x1c0c6 left at 1,353.7 units a
/// second under Unrelenting Force (thuum 20261111-223256 on c1), one step in
/// 0.44 s, and this in 2.2 s. The allowance's content is still the prop's
/// one clock's: an epoch rise (a grant, a return, a wake) raises how much of
/// the time that clock has run since the row's last write its first sample
/// may spend at once, never the time, so the mark is the epoch's own (no
/// sample yet at it), a refused first sample keeps it, a later epoch's
/// replaces it, and nothing is banked: a crafted seat that raises the epoch
/// as fast as it can is held to the bound of one that never does. The record's
/// travel from a grant on a new row is at most the cap's pace for the
/// server's elapsed time and `PROP_CLOCK_SLACK_MS`, plus this once, and over
/// any window rung 2's bound stands (`PROP_CLOCK_SLACK_MS`'s: a first sample
/// takes this at most of the time the clock has run, its lag included): a
/// lie is a hop of 3,000 units, never a teleport (ADR-063's H1 accepts the
/// lie class on a friends' server). UNMEASURED: x-prop-impact v1 reads each
/// caster's impact-to-first-sample time against half a second at the cap.
pub const PROP_FIRST_SAMPLE_UNITS: f32 = PROP_MAX_SPEED_UNITS_PER_S / 2.0;

/// How far ahead of the server's clock the step budget's own may run
/// (`PropRow::allowance_ms`; physics rung 2). Between two samples of one
/// epoch the budget refills by the owner's own clock (`SampleFacts::t_ms`,
/// its read of the body), so an honest stream whose arrivals bunch (two
/// samples 100 ms apart on the owner's clock reaching the server 5 ms apart)
/// is taken whole where the server's clock alone refuses the second; and
/// whatever the owner's clock claims, the budget's never runs past the
/// server's by more than this, so over any window of the server's time the
/// record travels no farther than the cap allows for that window,
/// PROP_CLOCK_LAG_MS and this slack, plus the one step (14,100 units in a
/// second, 8,100 at one instant). Physics rung 3a leaves those as they are:
/// an epoch's first sample takes at most `PROP_FIRST_SAMPLE_UNITS` of the
/// time the clock has run, its lag included, and brings a lagging clock to
/// the server's, so from a grant on a new row (no lag yet) the bound is the
/// cap for the elapsed time and this slack plus `PROP_FIRST_SAMPLE_UNITS`
/// (10,500 units in a second). The clock is the prop's, as the allowance
/// is: an epoch rise starts no fresh slack. The virtual scheduling of the
/// generic cell rate algorithm, its tolerance this slack (accessible:
/// "Generic cell rate algorithm", Wikipedia; foundational: ITU-T
/// Recommendation I.371, "Traffic control and congestion control in
/// B-ISDN"). UNMEASURED: x-prop-follow's loss leg reads the arrival jitter it
/// must forgive.
pub const PROP_CLOCK_SLACK_MS: u64 = 250;
/// How far behind the server's clock the step budget's own may fall
/// (physics rung 2): samples delayed in flight (a stall, then a bunch) leave
/// it behind, and the bunch takes the stalled time back on the owner's
/// clock; past this lag the server's own time refills it, so an owner whose
/// clock stands still while the server's runs banks no more than this and
/// the slack to spend later (a credit counted from an epoch's first sample
/// banked the whole epoch: the build plan's first form). UNMEASURED: the
/// longest stall x-prop-follow's loss leg reads.
pub const PROP_CLOCK_LAG_MS: u64 = 1_000;

/// What the core knows about a simulator's sample of a prop.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SampleFacts {
    /// The prop's reference id.
    pub refr: u32,
    /// The sender's actor id.
    pub player: u32,
    /// The owner epoch of the word the sender holds (ADR-066 decision 3; 0
    /// for a prop that never had a word): the sample is judged by it first.
    pub epoch: u32,
    /// The sender's sample number within (refr, epoch), from 1, wrapping
    /// 65535 to 1 (0 is the server's record answer, never a sample).
    pub seq: u16,
    /// The owner's own clock at the read, ms (its session clock): the step
    /// budget's refill between two samples of one epoch, bounded by the
    /// server's.
    pub t_ms: u32,
    /// The cell the sample's position lands in, 0 for none.
    pub cell: u32,
    /// The host of the cell the RECORD stands in, 0 while nobody hosts it.
    pub cell_host: u32,
    /// The body's reported state.
    pub asleep: bool,
    /// The sender's engine holds the body (Game.getPlayerGrabbedRef names it,
    /// Eli's D-PH3 A): an accepted sample sets the row's grab to it.
    pub held: bool,
    /// Units between the sample and the record.
    pub moved: f32,
    /// The server's wall clock, ms.
    pub now_ms: u64,
}

/// The touch a claim reports (the wire's `PropClaim.touch`).
pub mod touch {
    /// A contact of the player's character controller.
    pub const CONTACT: u8 = 0;
    /// The player grabbed the prop (the engine's grab, exclusive on one seat).
    pub const GRAB: u8 = 1;
    /// The player dropped it (a drop's reference is a prop from its first
    /// frame; a grabbed prop let go).
    pub const DROP: u8 = 2;
    /// A shout's or spell's impulse, and a trap's world body its seat's own
    /// script set moving (thuum D-S3 point 4; the core admits no other touch
    /// on a world body).
    pub const IMPULSE: u8 = 3;
}

/// One prop's row: who simulates it and on what terms.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PropRow {
    /// The simulating player's actor form id; 0 once the prop went back to
    /// the host (at rest, or the owner left or unloaded the prop's cell):
    /// the cell's host simulates, `owner_of`.
    pub owner: u32,
    /// The prop's owner epoch: up at every word (a grant, a return to the
    /// host, the host's wake), the wire's `PropOwner.epoch`.
    pub epoch: u32,
    /// The cell the prop's record stands in, by the last claim or accepted
    /// transform.
    pub cell: u32,
    /// The body's last reported state; a resting row names no simulator (the
    /// host's by default).
    pub asleep: bool,
    /// The owner's engine holds the prop (a grab since its last claim, or
    /// its last accepted sample said so, no release or rest yet): another
    /// player's claim is refused as Held. The holder's client reads its
    /// engine's grab at every sample (Game.getPlayerGrabbedRef, D-PH3 A) and
    /// sends it (`SampleFacts::held`), claims a grab only while the engine
    /// names the body (havok-props.md E43) and never rests a held body on the
    /// wire (A6), so its first held-false sample ends the hold and its asleep
    /// sample is a hold already ended too.
    pub grabbed: bool,
    /// The last accepted sample's time (the grant's before any): the moving
    /// window's clock.
    pub last_sample_ms: u64,
    /// The step budget: a token bucket on distance, in units, as it stood at
    /// `allowance_ms`. An accepted sample spends the units it moved the
    /// record and leaves at most `PROP_STEP_ALLOWANCE_UNITS`; time refills it
    /// at `PROP_MAX_SPEED_UNITS_PER_S` up to `PROP_FIRST_SAMPLE_UNITS` for an
    /// epoch's first sample (`last_seq` 0, physics rung 3a) and up to
    /// `PROP_STEP_ALLOWANCE_UNITS` for a later one; a refused sample spends
    /// nothing and moves no clock. It is the prop's, not its owner's: a grant,
    /// a host's wake or a return leaves it as it stands, so seats trading a
    /// prop by claims cannot each start with a full one, and an epoch's first
    /// sample spends only the time the prop's clock has run.
    pub allowance: f32,
    /// The step budget's clock, on the server's timeline: when `allowance`
    /// was last brought up to date (a new row starts full, at its first
    /// moment). An epoch's first sample brings it to the server's clock, as
    /// before rung 2; each later one moves it by the owner's elapsed time
    /// since its last accepted sample (`SampleFacts::t_ms`), held between
    /// `PROP_CLOCK_LAG_MS` behind the server's clock and
    /// `PROP_CLOCK_SLACK_MS` ahead of it. It never runs back, and an epoch
    /// rise keeps it, as it keeps the allowance.
    pub allowance_ms: u64,
    /// The last accepted sample's sequence at this epoch, 0 before any: a
    /// sample not newer is Late. Every epoch rise sets it to 0.
    pub last_seq: u16,
    /// The owner's clock at the last accepted sample of this epoch, the
    /// measure of the next one's elapsed time; meaningful once `last_seq`
    /// is.
    pub last_t_ms: u32,
}

/// The answer to a claim (ADR-058 decision 1; A2, A3, A7).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ClaimVerdict {
    /// The claimant simulates the prop from now, at this epoch; the word
    /// (`PropOwner`) goes to every listener.
    Granted(u32),
    /// Another player holds the prop grabbed; nothing changes.
    Held,
    /// The claimant simulates it already (a live row); nothing changes but
    /// the grab mark.
    Same,
    /// Another seat streams the prop awake right now and the claimant's
    /// touch is no grab (A3: a non-owner's contact never takes a moving
    /// prop; a trap body's second firing is the same row, the first firing
    /// wins); nothing changes, the claimant's copy stays on the stream.
    Moving,
    /// The claimant's game reported the prop's cell unloaded and has not
    /// reported it loaded since, nor had the cell back in its reach
    /// (`unloaded`, `loaded`, `reached`; ADR-071's D-PH2 A): its 3D of the
    /// prop is gone, so a claim from it there is a late one; nothing changes.
    Away,
}

/// The answer to a pickup (ADR-071 decision 2's table: a pickup is a take,
/// so it reads the claim's rows for a touch that is no grab).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PickupVerdict {
    /// Nothing refuses it: the picker's own row, or a prop at rest, stalled
    /// or unclaimed; the core gives the item as before.
    Taken,
    /// Another player holds the prop grabbed (A2: nothing beats a grab);
    /// nothing changes.
    Held,
    /// Another seat streams the prop awake inside the moving window (A3: a
    /// grab alone takes a moving prop, and a pickup is no grab); nothing
    /// changes, the picker's copy stays on the stream.
    Moving,
}

/// The answer to a simulator's transform (ADR-058 decision 2; A4, A7; ADR-071
/// decision 6, physics rung 2).
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum TransformVerdict {
    /// The sender simulates the prop: the record takes the transform.
    Accepted,
    /// Another seat simulates it at the prop's epoch (the owner, or the
    /// cell's host by default).
    NotOwner,
    /// The sample's point stands in no cell (ADR-071 decision 6's
    /// conservation: no record outside a cell): refused, the record stands,
    /// whoever sent it.
    Outside,
    /// An awake sample farther from the record than the step budget allows
    /// (the row's allowance, `PropRow::allowance`), or not a number:
    /// refused, the record stands.
    TooFast,
    /// Taken, and the body's rest returned the prop to the cell's host at
    /// this epoch, the host's own rest and a holder's included: the word
    /// `PropOwner` with player 0 goes to every listener (A4: ownership
    /// returns to the host only at rest on the owner's screen, by the
    /// owner's own final sample, and every return is a word, so no seat is
    /// left holding an older word).
    Returned(u32),
    /// An asleep sample farther than the step budget allows, clamped and
    /// taken (ADR-071 decision 6: a live owner's rest past the bucket is
    /// clamped and accepted): the record moves `allowed` units from where it
    /// stands toward the sample, which the budget spends, and the prop is the
    /// host's again at `epoch` (the word goes out); nothing here knows a
    /// position, so the core places the point, relays the rest there and
    /// answers the sender with the record, and every seat ends at it. It
    /// replaces rung 1's RestRefused, the rest taken and the record left
    /// standing; either way a refused final sample never leaves a live row
    /// nobody can move.
    Clamped {
        /// The units the record moves toward the sample: what the allowance
        /// held, 0 for a distance that is not a number.
        allowed: f32,
        /// The prop's epoch at the return.
        epoch: u32,
    },
    /// Woken by the host: its awake sample of a prop that was not its live
    /// row (a resting prop its engine moved): the row is the host's at this
    /// new epoch and the word names the host BY ID, so every seat's word
    /// names the seat that streams and a host change leaves the old host
    /// streaming until its rest (A7; the review of 2026-11-03: a word of
    /// player 0 over a row naming the host stranded the prop after a host
    /// change, nobody simulating it).
    Woken(u32),
    /// A wake by a sender whose game reported the sample's cell unloaded
    /// and has not reported it loaded since, nor had the cell back in its
    /// reach: an unloading host still in its grace waking the prop it just
    /// left (the review of 2026-11-03); a live owner's own stream passes;
    /// nothing changes.
    Away,
    /// A resting prop in a cell nobody hosts (no live row, no host): it
    /// freezes at the record (the design law: an unhosted cell freezes);
    /// refused.
    Frozen,
    /// The sample names an epoch below the prop's: a sample of an earlier
    /// tenure (an old owner's in flight, a woken host's sent before its wake
    /// word reached it). Dropped whoever sent it, never read as a pass or a
    /// move (ADR-066 decision 4); its own epoch's grace (ADR-066 decision 5)
    /// is not built.
    Stale,
    /// The sample names an epoch above the prop's, a word the server never
    /// said: dropped whoever sent it.
    Future,
    /// The owner's sample at the prop's epoch whose sequence is not newer
    /// than the row's last accepted one there (a repeat, or an unreliable
    /// sample overtaken in flight): dropped, no fault.
    Late,
}

/// Every prop's row and every prop's epoch (kept after its row is gone).
#[derive(Debug, Default)]
pub struct Props {
    rows: HashMap<u32, PropRow>,
    epochs: HashMap<u32, u32>,
    /// (player, cell) pairs a player's game reported unloaded and not
    /// loaded again since, nor had back in its reach: an unload is a state,
    /// not a moment, so the player's grants and wakes there are refused
    /// (Away) until its load report or the cell's return to its reach; a
    /// gate on the seat's own report, not on a position (ADR-063 decision 4).
    away: HashSet<(u32, u32)>,
}

impl PropRow {
    /// A new row naming `owner` at `epoch` in `cell` at `now_ms`: awake, no
    /// grab, no sample yet at its epoch, and its allowance full for its first
    /// sample, `PROP_FIRST_SAMPLE_UNITS` (the record's last write is no row's,
    /// so no clock of the prop's has run short of it): one row a prop, once.
    fn new(owner: u32, epoch: u32, cell: u32, now_ms: u64) -> Self {
        PropRow {
            owner,
            epoch,
            cell,
            asleep: false,
            grabbed: false,
            last_sample_ms: now_ms,
            allowance: PROP_FIRST_SAMPLE_UNITS,
            allowance_ms: now_ms,
            last_seq: 0,
            last_t_ms: 0,
        }
    }

    /// The row at a new epoch (every word: a grant, a return, a wake, a
    /// clamp, an unload, a departure): no sample yet at it, so the next one's
    /// sequence and clock are measured from nothing, and its allowance may
    /// hold up to `PROP_FIRST_SAMPLE_UNITS` of the prop's clock (physics rung
    /// 3a); the allowance and its clock stand (they are the prop's), so the
    /// rise itself buys no time.
    fn begin(&mut self, epoch: u32) {
        self.epoch = epoch;
        self.last_seq = 0;
        self.last_t_ms = 0;
    }

    /// Whether the row names its owner: held, or awake. A resting row and an
    /// owner of 0 never do (the host's by default), and no clock lapses a
    /// row (ADR-071 decision 2: A1 to A7 with no timers).
    fn live(&self) -> bool {
        self.owner != 0 && (self.grabbed || !self.asleep)
    }

    /// Whether another seat streams the prop awake right now: live, not
    /// held, the last accepted sample inside the moving window.
    fn moving(&self, now_ms: u64) -> bool {
        self.live() && !self.grabbed && now_ms < self.last_sample_ms.saturating_add(PROP_MOVING_MS)
    }

    /// The allowance for a sample arriving at `now_ms`, read at `t_ms` on its
    /// owner's clock, and the step budget's clock it is refilled to
    /// (`allowance_ms`): an epoch's first sample by the server's elapsed time
    /// (its owner's clock has no earlier read here to measure from), a later
    /// one by its owner's elapsed time since the last accepted sample (none
    /// for a clock that ran backwards: no wrapping arithmetic touches
    /// `t_ms`), the clock then held between `PROP_CLOCK_LAG_MS` behind the
    /// server's and `PROP_CLOCK_SLACK_MS` ahead of it. The stored units
    /// refill at `PROP_MAX_SPEED_UNITS_PER_S` for the clock's advance, to at
    /// most `PROP_FIRST_SAMPLE_UNITS` for an epoch's first sample (physics
    /// rung 3a: the tenure's first read of a body that flew while the word
    /// travelled) and `PROP_STEP_ALLOWANCE_UNITS` for a later one: the same
    /// clock either way, only how much of its time one sample may spend.
    fn allowance_for(&self, now_ms: u64, t_ms: u32) -> (f32, u64) {
        let first = self.last_seq == 0;
        let elapsed_ms = if first {
            now_ms.saturating_sub(self.allowance_ms)
        } else {
            t_ms.checked_sub(self.last_t_ms).map_or(0, u64::from)
        };
        let clock_ms = self
            .allowance_ms
            .saturating_add(elapsed_ms)
            .max(now_ms.saturating_sub(PROP_CLOCK_LAG_MS))
            .min(now_ms.saturating_add(PROP_CLOCK_SLACK_MS))
            .max(self.allowance_ms);
        // the time base, clamped to ten seconds (half of one refills a first
        // sample's whole allowance)
        let refill_ms = u16::try_from(clock_ms.saturating_sub(self.allowance_ms).min(10_000)).unwrap_or(10_000);
        let most = if first { PROP_FIRST_SAMPLE_UNITS } else { PROP_STEP_ALLOWANCE_UNITS };
        let allowed = (self.allowance + PROP_MAX_SPEED_UNITS_PER_S * f32::from(refill_ms) / 1_000.0).min(most);
        (allowed, clock_ms)
    }

    /// What an accepted sample leaves the allowance, brought to `clock_ms`:
    /// what it held less the units it moved the record (a distance, never
    /// below 0), and never more than one step (physics rung 3a: a first
    /// sample's half second is that sample's alone, so the tenure's next
    /// sample is judged by one step again and nothing it left over carries to
    /// a later epoch's first).
    fn spend(&mut self, allowed: f32, moved: f32, clock_ms: u64) {
        self.allowance = (allowed - moved.max(0.0)).clamp(0.0, PROP_STEP_ALLOWANCE_UNITS);
        self.allowance_ms = clock_ms;
    }
}

/// Whether the sample numbered `seq` is newer than `last`, the row's last
/// accepted one at the epoch (0: none yet), by serial-number arithmetic on
/// the u16: ahead by 1 to 32,767 (accessible: Fiedler, "Reliable Ordered
/// Messages", gafferongames.com, 2016, its 16-bit sequence buffers;
/// foundational: Elz and Bush, "Serial Number Arithmetic", RFC 1982, 1996).
/// It is the one deliberate wrap on a client-supplied number (thuum's
/// .claude/rules/rust.md: wrapping is never the intended behavior on one),
/// since one tenure of one prop at 10 Hz passes 65,535 samples in 1.8 hours
/// and the sender wraps 65535 to 1. A sequence of 0 is the server's record
/// answer, which no seat sends (wire-validate refuses it from a client), so
/// it is never newer.
fn newer(seq: u16, last: u16) -> bool {
    seq != 0 && (last == 0 || (1..=32_767).contains(&seq.wrapping_sub(last)))
}

impl Props {
    /// Who simulates `refr`, given that `cell_host` hosts the cell its record
    /// stands in (0 while nobody does): the row's owner while the row is live
    /// (awake or held), else the host. No clock: the words alone move it (A1).
    #[must_use]
    pub fn owner_of(&self, refr: u32, cell_host: u32) -> u32 {
        match self.rows.get(&refr) {
            Some(row) if row.live() => row.owner,
            _ => cell_host,
        }
    }

    /// The prop's owner epoch: the last word's, 0 before any.
    #[must_use]
    pub fn epoch_of(&self, refr: u32) -> u32 {
        self.epochs.get(&refr).copied().unwrap_or(0)
    }

    /// The row's owner while the row is live, else 0: who the core's word to
    /// a late listener names (with the epoch; 0 names the host by default).
    #[must_use]
    pub fn live_owner(&self, refr: u32) -> u32 {
        match self.rows.get(&refr) {
            Some(row) if row.live() => row.owner,
            _ => 0,
        }
    }

    /// Whether the row's owner's engine holds the prop (a live row's grab):
    /// the `held` of the core's record answer.
    #[must_use]
    pub fn held(&self, refr: u32) -> bool {
        self.rows.get(&refr).is_some_and(|row| row.live() && row.grabbed)
    }

    /// `player` touched `refr`, which stands in `cell`, by `touch` at
    /// `now_ms` (A2, A3): the claimant that simulates it already keeps it
    /// (Same, the grab mark from the touch); another player's grab holds it
    /// against everything (Held); a prop another seat streams awake is nobody
    /// else's by a contact, a drop or an impulse (Moving) and a grab takes it
    /// (a grabber's engine holds its own copy, which a Keyframed copy on the
    /// stream never lets it do, havok-props.md E28 and E31, so the case is a
    /// race at rest); a prop at rest, stalled or unclaimed is the newest
    /// touch's (Granted, the epoch up, the word to every listener). The
    /// claimant's cell and distance are no gate (ADR-063 decision 4); a claim
    /// from a game that unloaded the cell is late (Away). A grab ends the
    /// claimant's hold of any other prop first, whatever its own verdict: the
    /// engine holds one grabbed object per player (its grabbedObject,
    /// CommonLibSSE-NG P/PlayerCharacter.h:697), so a new grab means the old
    /// hold is over, whether or not the server heard it end (a release inside
    /// the grab claim's round trip, the engine's own drop, havok-props.md
    /// E41), and no seat holds two props at once.
    pub fn claim(&mut self, refr: u32, player: u32, cell: u32, touch: u8, now_ms: u64) -> ClaimVerdict {
        if touch == touch::GRAB {
            // the grab mark alone goes: the old row stays the player's awake
            // row, streamed or, past PROP_MOVING_MS from its last sample (not
            // refreshed here), stalled and anyone's to claim
            for (other, row) in &mut self.rows {
                if *other != refr && row.owner == player && row.grabbed {
                    row.grabbed = false;
                }
            }
        }
        if self.away.contains(&(player, cell)) {
            return ClaimVerdict::Away;
        }
        if let Some(row) = self.rows.get_mut(&refr) {
            if row.live() {
                if row.owner == player {
                    row.grabbed = touch == touch::GRAB;
                    row.cell = cell;
                    // a touch wakes the row: the holder's release is followed
                    // by its fall samples
                    row.asleep = false;
                    row.last_sample_ms = now_ms;
                    return ClaimVerdict::Same;
                }
                if row.grabbed {
                    return ClaimVerdict::Held;
                }
                if touch != touch::GRAB && row.moving(now_ms) {
                    return ClaimVerdict::Moving;
                }
            }
        }
        let epoch = self.epoch_of(refr).saturating_add(1);
        self.epochs.insert(refr, epoch);
        // the allowance and its clock are the prop's: a grant keeps them as
        // they stand (its first sample spends what that clock has run since,
        // up to half a second at the cap), a new row starts full
        let (allowance, allowance_ms) = self
            .rows
            .get(&refr)
            .map_or((PROP_FIRST_SAMPLE_UNITS, now_ms), |row| (row.allowance, row.allowance_ms));
        self.rows.insert(refr, PropRow { grabbed: touch == touch::GRAB, allowance, allowance_ms, ..PropRow::new(player, epoch, cell, now_ms) });
        ClaimVerdict::Granted(epoch)
    }

    /// `player` picks `refr` up at `now_ms`: its activation of the item
    /// reference, which gives the item and deletes a server-made reference
    /// for every seat, the release of a grab the engine refused in its own
    /// frame among them (thuum docs/engine/havok-props.md E16 and E28). A
    /// pickup is a take (ADR-071 decision 2's table), read off the claim's
    /// rows for a touch that is no grab: another player's hold refuses it
    /// (Held), another seat's awake stream inside the moving window refuses
    /// it (Moving), and the picker's own row and a prop at rest, stalled or
    /// unclaimed are taken. It changes no row and asks no unload state (Away
    /// refuses a late grant, and a pickup grants nothing).
    #[must_use]
    pub fn pickup(&self, refr: u32, player: u32, now_ms: u64) -> PickupVerdict {
        match self.rows.get(&refr) {
            Some(row) if row.owner == player => PickupVerdict::Taken,
            Some(row) if row.live() && row.grabbed => PickupVerdict::Held,
            Some(row) if row.moving(now_ms) => PickupVerdict::Moving,
            _ => PickupVerdict::Taken,
        }
    }

    /// `player` reports `refr`'s transform, landing in `cell` (0 for none)
    /// whose record's cell is hosted by `cell_host` (0 while nobody), the
    /// body `asleep` or not, `held` or not, `moved` units from the record, at
    /// `now_ms`, under the word's `epoch`, numbered `seq`, read at `t_ms` on
    /// its owner's clock. A point in no cell is refused whoever sends it
    /// (Outside), a resting prop in a cell nobody hosts freezes (Frozen), and
    /// a wake from a game that reported the cell unloaded waits (Away); then
    /// by the epoch first and the owner table after (ADR-066 decision 4):
    /// below the prop's epoch Stale, above it Future, another seat's at it
    /// NotOwner; then by the order, a sequence not newer than the row's last
    /// at the epoch Late; then inside the step budget (the row's allowance,
    /// refilled by the owner's clock bounded by the server's, which the step
    /// then spends; the epoch's first sample may spend up to half a second of
    /// that clock at once, physics rung 3a), an awake sample past it TooFast.
    /// Every asleep sample
    /// returns the prop to the host at once, the epoch up (Returned: the core
    /// sends the word with player 0), the host's own rest and a holder's
    /// included, and one past the budget is clamped to it (Clamped, the same
    /// return); a host's awake sample of a prop that is not its live row
    /// wakes it (Woken: the row the host's at a new epoch, the word naming the
    /// host by id). An accepted awake sample sets the row's grab to its
    /// `held`. A live owner's own stream passes wherever it lands.
    pub fn transform(&mut self, f: &SampleFacts) -> TransformVerdict {
        let SampleFacts { refr, player, epoch: worded, seq, t_ms, cell, cell_host, asleep, held, moved, now_ms } = *f;
        if cell == 0 {
            return TransformVerdict::Outside;
        }
        let owner = self.owner_of(refr, cell_host);
        if owner == 0 {
            return TransformVerdict::Frozen;
        }
        // a moving body's owner never depends on its cell (ADR-071 decision
        // 2), so the unload's state gates a WAKE alone: a live owner
        // streaming its own row into a cell its game reported unloaded (a
        // missed load report) keeps streaming, its row never stranded; a
        // host's sample that would take a resting row there waits for the
        // load report or the cell's return to its reach
        let own_live = self.rows.get(&refr).is_some_and(|r| r.live() && r.owner == player);
        if !own_live && self.away.contains(&(player, cell)) {
            return TransformVerdict::Away;
        }
        // by its epoch first, then by the owner table, never by its position
        // (ADR-066 decision 4): a sample of another tenure changes nothing,
        // whoever sent it
        let epoch = self.epoch_of(refr);
        if worded < epoch {
            return TransformVerdict::Stale;
        }
        if worded > epoch {
            return TransformVerdict::Future;
        }
        if owner != player {
            return TransformVerdict::NotOwner;
        }
        let row = self.rows.get(&refr);
        if !newer(seq, row.map_or(0, |r| r.last_seq)) {
            return TransformVerdict::Late;
        }
        // the step budget: whatever the sample rate, whatever the owner's
        // clock claims and however often the epoch rises, the record travels
        // at most the cap's pace over the server's time and the clock's
        // slack, plus one first sample's allowance (a prop with no row yet
        // holds that whole: its next sample is its epoch's first)
        let (allowed, clock_ms) = row.map_or((PROP_FIRST_SAMPLE_UNITS, now_ms), |r| r.allowance_for(now_ms, t_ms));
        // one hand per player (CommonLibSSE-NG P/PlayerCharacter.h:697): a
        // held sample takes the hand only while the player holds nothing
        // else, so a late sample of a hold its next grab ended (the claim
        // moves the hand) never takes it back
        let hand_elsewhere = self.rows.iter().any(|(&other, r)| other != refr && r.owner == player && r.grabbed);
        if !moved.is_finite() || moved > allowed {
            if !asleep {
                return TransformVerdict::TooFast;
            }
            // a rest past the budget is clamped and taken (ADR-071 decision
            // 6): the record moves what the budget held toward the sample,
            // the step spends it, and the prop is the host's again, so a
            // refused final sample never leaves a live row nobody can move
            let step = if moved.is_finite() { allowed } else { 0.0 };
            let next = epoch.saturating_add(1);
            self.epochs.insert(refr, next);
            let row = self.rows.entry(refr).or_insert_with(|| PropRow::new(owner, epoch, cell, now_ms));
            row.owner = 0;
            row.grabbed = false;
            row.asleep = true;
            row.last_sample_ms = now_ms;
            row.spend(allowed, step, clock_ms);
            row.begin(next);
            return TransformVerdict::Clamped { allowed: step, epoch: next };
        }
        let row = self.rows.entry(refr).or_insert_with(|| PropRow::new(owner, epoch, cell, now_ms));
        // the host's sample of a prop that was not its live row (a resting
        // one) makes it the host's row
        row.owner = owner;
        row.cell = cell;
        row.asleep = asleep;
        row.last_sample_ms = now_ms;
        // the step spends what it moved the record, leaving one step at most
        row.spend(allowed, moved, clock_ms);
        row.last_seq = seq;
        row.last_t_ms = t_ms;
        if asleep {
            // the body rests: the prop is the host's again by this very
            // sample, the host's own rest and a holder's included (a held
            // body never rests on the wire, A6, so the holder's asleep
            // sample is a hold its engine already ended, D-PH3 A), and every
            // listener hears it (a seat holding an older word would
            // otherwise keep it)
            row.owner = 0;
            row.grabbed = false;
            let next = epoch.saturating_add(1);
            row.begin(next);
            self.epochs.insert(refr, next);
            return TransformVerdict::Returned(next);
        }
        // the engine's own grab, read at every sample (D-PH3 A on the wire):
        // a held sample keeps the row the holder's (another seat's claim is
        // Held), the first held-false one ends the hold and the row moves on
        // under its owner until its rest returns it (A4)
        row.grabbed = held && !hand_elsewhere;
        if !own_live {
            // the host's engine moved a prop nobody streamed: the word names
            // the host by id, so the seat that streams is the seat every
            // word names (a host change leaves it streaming until its rest)
            let next = epoch.saturating_add(1);
            row.begin(next);
            self.epochs.insert(refr, next);
            return TransformVerdict::Woken(next);
        }
        TransformVerdict::Accepted
    }

    /// `player`'s game unloaded `cell` (a door, a teleport, a square leaving
    /// its grid): its 3D of every prop there is gone, and the client's rescan
    /// untracks the prop before any final sample (HYPOTHESIS: the client's
    /// order, until a run), so every live row of its in the cell, a held one
    /// included, falls back to the host by default at a new epoch and the
    /// core sends the word (player 0) for each, as a departure's would; a
    /// resting row names nobody already. The unload is a state, not a moment
    /// (ADR-071's D-PH2 A): until `loaded` or `reached`, the player's grants
    /// and wakes in the cell are refused (Away), since an unloading host
    /// still hosts through its grace and its late sample would wake the prop
    /// by id again. Returns (refr, epoch) per returned prop, sorted.
    pub fn unloaded(&mut self, player: u32, cell: u32) -> Vec<(u32, u32)> {
        self.away.insert((player, cell));
        let mut returned = Vec::new();
        for (&refr, row) in self.rows.iter_mut().filter(|(_, r)| r.cell == cell && r.owner == player) {
            row.owner = 0;
            row.grabbed = false;
            row.asleep = true;
            let next = row.epoch.saturating_add(1);
            row.begin(next);
            returned.push((refr, next));
        }
        for &(refr, epoch) in &returned {
            self.epochs.insert(refr, epoch);
        }
        returned.sort_unstable();
        returned
    }

    /// `player` left the server: every row still naming it (awake or held)
    /// falls back to its cell's host at a new epoch, and the core sends the
    /// word (player 0) for each; a resting row of its was the host's already
    /// (its rest's word said so). Its unload states go with it. Returns
    /// (refr, epoch) per returned prop, sorted.
    pub fn gone(&mut self, player: u32) -> Vec<(u32, u32)> {
        self.away.retain(|(p, _)| *p != player);
        let mut returned = Vec::new();
        for (&refr, row) in self.rows.iter_mut().filter(|(_, r)| r.owner == player) {
            row.owner = 0;
            row.grabbed = false;
            row.asleep = true;
            let next = row.epoch.saturating_add(1);
            row.begin(next);
            returned.push((refr, next));
        }
        for &(refr, epoch) in &returned {
            self.epochs.insert(refr, epoch);
        }
        returned.sort_unstable();
        returned
    }

    /// `player`'s game reported `cell` loaded: its claims and samples there
    /// are judged again (the end of `unloaded`'s state).
    pub fn loaded(&mut self, player: u32, cell: u32) {
        self.away.remove(&(player, cell));
    }

    /// `cells` are in `player`'s reach now (the core's RefreshReach at a
    /// square change): the unload's state ends for each, the way a follow
    /// vouches by loaded or reach; a seat whose load report for a neighbour
    /// never fired (playtest 41: 9731 missing from c1's loaded set for 57 s)
    /// is not refused there for good.
    pub fn reached(&mut self, player: u32, cells: &[u32]) {
        for &cell in cells {
            self.away.remove(&(player, cell));
        }
    }

    /// Every row, by reference id, for the lab's reading of the server.
    #[must_use]
    pub fn rows(&self) -> Vec<(u32, PropRow)> {
        let mut rows: Vec<(u32, PropRow)> = self.rows.iter().map(|(&refr, row)| (refr, *row)).collect();
        rows.sort_unstable_by_key(|r| r.0);
        rows
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    const CELL: u32 = 0x9732;
    const BASKET: u32 = 0x2ad9c;
    const C1: u32 = 0xff00_0002;
    const C2: u32 = 0xff00_0003;
    const C3: u32 = 0xff00_0004;

    // a sample's every fact, as the core gathers them
    #[allow(clippy::too_many_arguments)]
    fn facts(refr: u32, player: u32, epoch: u32, seq: u16, t_ms: u32, cell: u32, cell_host: u32, asleep: bool, held: bool, moved: f32, now_ms: u64) -> SampleFacts {
        SampleFacts { refr, player, epoch, seq, t_ms, cell, cell_host, asleep, held, moved, now_ms }
    }

    impl Props {
        /// A sample from a seat that heard every word, its engine read at the
        /// server's moment (rung 1's cases on v2's terms): the prop's epoch,
        /// the next sequence there (the row's last accepted one and one), its
        /// own clock at the server's, and the row's grab as its hold while
        /// awake (a holder's engine still holds the body; nobody holds a body
        /// at rest, A6).
        #[allow(clippy::too_many_arguments)]
        fn sample(&mut self, refr: u32, player: u32, cell: u32, cell_host: u32, asleep: bool, moved: f32, now_ms: u64) -> TransformVerdict {
            let row = self.rows.get(&refr);
            let seq = row.map_or(1, |r| r.last_seq.checked_add(1).unwrap_or(1));
            let held = !asleep && row.is_some_and(|r| r.owner == player && r.grabbed);
            let t_ms = u32::try_from(now_ms).unwrap_or(u32::MAX);
            let epoch = self.epoch_of(refr);
            self.transform(&facts(refr, player, epoch, seq, t_ms, cell, cell_host, asleep, held, moved, now_ms))
        }
    }

    #[test]
    fn an_unclaimed_prop_is_the_hosts_and_nobodys_in_an_unhosted_cell() {
        let p = Props::default();
        assert_eq!(p.owner_of(BASKET, C1), C1);
        assert_eq!(p.owner_of(BASKET, 0), 0);
        assert_eq!(p.epoch_of(BASKET), 0);
    }

    #[test]
    fn a_touch_at_rest_is_granted_the_same_toucher_keeps_it_and_a_moving_prop_is_nobody_elses_by_a_touch() {
        // A3 (ADR-063's props note, ADR-071 decision 2): a non-owner's contact
        // never takes a moving prop; at rest or stalled the newest touch wins
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.owner_of(BASKET, C1), C2);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_100), ClaimVerdict::Same);
        assert_eq!(p.epoch_of(BASKET), 1);
        // c1 walks into it while c2 streams it (the grant counts as a sample): refused
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_200), ClaimVerdict::Moving);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 10.0, 1_500), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::IMPULSE, 1_600), ClaimVerdict::Moving);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::DROP, 1_700), ClaimVerdict::Moving);
        assert_eq!(p.owner_of(BASKET, C1), C2);
        assert_eq!(p.epoch_of(BASKET), 1);
        // the stream dies without its asleep sample: past the moving window the next touch takes it
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_500 + PROP_MOVING_MS - 1), ClaimVerdict::Moving);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_500 + PROP_MOVING_MS), ClaimVerdict::Granted(2));
        assert_eq!(p.owner_of(BASKET, C2), C1);
        // another prop counts its own epochs
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 3_000), ClaimVerdict::Granted(1));
        assert_eq!(p.epoch_of(BASKET), 2);
    }

    #[test]
    fn a_grab_beats_a_touch_and_nothing_beats_a_grab() {
        let mut p = Props::default();
        // c2 streams it awake; c1's grab takes it (a grab beats a touch)
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.claim(BASKET, C1, CELL, touch::GRAB, 1_100), ClaimVerdict::Granted(2));
        assert_eq!(p.owner_of(BASKET, C2), C1);
        // held: every other seat's touch, grab included, is Held
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_200), ClaimVerdict::Held);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::IMPULSE, 1_300), ClaimVerdict::Held);
        assert_eq!(p.claim(BASKET, C3, CELL, touch::GRAB, 1_400), ClaimVerdict::Held);
        // no clock ends a hold (D-PH3 A: no grab cap): ten minutes after the
        // holder's last sample the prop is still in its hand
        assert_eq!(p.sample(BASKET, C1, CELL, C2, false, 1.0, 1_500), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 601_500), ClaimVerdict::Held);
        assert_eq!(p.live_owner(BASKET), C1);
        // the holder's release clears the grab (its client's drop claim when
        // the engine lets go, by a key or by itself: havok-props.md E40 and
        // E41); the fall streams, a contact on it is moving, and the rest
        // returns it to the host with the word
        assert_eq!(p.claim(BASKET, C1, CELL, touch::DROP, 601_600), ClaimVerdict::Same);
        assert_eq!(p.sample(BASKET, C1, CELL, C2, false, 10.0, 601_700), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 601_750), ClaimVerdict::Moving);
        assert_eq!(p.sample(BASKET, C1, CELL, C2, true, 10.0, 601_800), TransformVerdict::Returned(3));
        assert_eq!(p.owner_of(BASKET, C2), C2);
        assert_eq!(p.epoch_of(BASKET), 3);
        // a hold whose release claim never came ends at the holder's rest: a
        // held body never rests on the wire (A6), so the asleep sample is a
        // hold already ended, and the word goes out
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.sample(BASKET, C2, CELL, C1, false, 10.0, 1_500), TransformVerdict::Accepted);
        assert_eq!(q.sample(BASKET, C2, CELL, C1, true, 10.0, 2_000), TransformVerdict::Returned(2));
        assert_eq!(q.live_owner(BASKET), 0);
        assert_eq!(q.claim(BASKET, C3, CELL, touch::CONTACT, 2_100), ClaimVerdict::Granted(3));
        // a holder that leaves is named by nobody: its departure words the row
        let mut u = Props::default();
        assert_eq!(u.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(u.gone(C2), vec![(BASKET, 2)]);
        assert_eq!(u.owner_of(BASKET, C1), C1);
        // a grab of a prop the claimant streams already is its own row's: Same, held from there
        let mut r = Props::default();
        assert_eq!(r.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(r.claim(BASKET, C2, CELL, touch::GRAB, 1_100), ClaimVerdict::Same);
        assert_eq!(r.claim(BASKET, C1, CELL, touch::CONTACT, 5_000), ClaimVerdict::Held);
    }

    #[test]
    fn a_players_new_grab_ends_its_hold_of_another_prop() {
        // the engine holds one grabbed object per player (CommonLibSSE-NG P/PlayerCharacter.h:697): a new grab means the
        // old hold is over, whether or not its release reached the server (the re-check of 46a7bfa4: refusing the new
        // grab contradicted the engine), so the grab is judged as any grab and the old row loses its grab mark alone
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        // the second grab: granted, the first hold ended
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::GRAB, 1_100), ClaimVerdict::Granted(1));
        assert_eq!(p.live_owner(0x54a44), C2);
        // the first stays c2's awake row, held no more: another seat's contact on it is Moving inside the window, from
        // its grant at 1_000 (the new grab refreshes no sample), not Held; past the window the stalled row is the next
        // touch's
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_200), ClaimVerdict::Moving);
        assert_eq!(p.live_owner(BASKET), C2);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_000 + PROP_MOVING_MS - 1), ClaimVerdict::Moving);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_000 + PROP_MOVING_MS), ClaimVerdict::Granted(2));
        // the new hold stands: another seat's touch is Held, the holder's grab of it again is Same
        assert_eq!(p.claim(0x54a44, C3, CELL, touch::CONTACT, 2_700), ClaimVerdict::Held);
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::GRAB, 2_750), ClaimVerdict::Same);
        assert_eq!(p.claim(0x54a44, C3, CELL, touch::CONTACT, 2_800), ClaimVerdict::Held);
        // a contact is no grab and ends nothing, and another player's grab is its own hand's
        assert_eq!(p.claim(0x1234, C2, CELL, touch::CONTACT, 2_900), ClaimVerdict::Granted(1));
        assert_eq!(p.claim(0x5678, C1, CELL, touch::GRAB, 3_000), ClaimVerdict::Granted(1));
        assert_eq!(p.claim(0x54a44, C3, CELL, touch::CONTACT, 3_050), ClaimVerdict::Held);
        // the holder's grab of a row it streams moves the hand there: that row is held, the last one takes a grab
        assert_eq!(p.claim(0x1234, C2, CELL, touch::GRAB, 3_100), ClaimVerdict::Same);
        assert_eq!(p.claim(0x1234, C3, CELL, touch::CONTACT, 3_150), ClaimVerdict::Held);
        assert_eq!(p.claim(0x54a44, C3, CELL, touch::GRAB, 3_200), ClaimVerdict::Granted(2));
    }

    #[test]
    fn a_pickup_is_a_take_refused_on_a_prop_another_seat_holds_or_moves() {
        // ADR-071 decision 2's table. A refused grab's key-up is a pickup (havok-props.md E16 and E28), and the
        // server's pickup read no prop row: in x-moving-press 20261112-082448 c1's key-up took the ingot c2 was moving,
        // for every seat (D1 of thuum's read of rung 1's premises). A pickup reads the claim's rows for a touch that is
        // no grab: another player's hold refuses it, as another seat's awake stream does; the picker's own row, a
        // stalled stream and a resting prop are taken; no verdict changes a row
        let mut p = Props::default();
        assert_eq!(p.pickup(BASKET, C1, 500), PickupVerdict::Taken);
        // c2 holds it: another seat's pickup is refused, ten minutes on too (no clock ends a hold); c2's own is taken
        assert_eq!(p.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.pickup(BASKET, C1, 1_100), PickupVerdict::Held);
        assert_eq!(p.pickup(BASKET, C2, 1_100), PickupVerdict::Taken);
        assert_eq!(p.pickup(BASKET, C3, 601_000), PickupVerdict::Held);
        // the release: the fall c2 streams awake is refused to another seat inside the moving window from its last
        // sample, and past it the stalled row is anyone's, as a contact's claim would be
        assert_eq!(p.claim(BASKET, C2, CELL, touch::DROP, 601_100), ClaimVerdict::Same);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 10.0, 601_200), TransformVerdict::Accepted);
        assert_eq!(p.pickup(BASKET, C1, 601_300), PickupVerdict::Moving);
        assert_eq!(p.pickup(BASKET, C2, 601_300), PickupVerdict::Taken);
        assert_eq!(p.pickup(BASKET, C1, 601_200 + PROP_MOVING_MS - 1), PickupVerdict::Moving);
        assert_eq!(p.pickup(BASKET, C1, 601_200 + PROP_MOVING_MS), PickupVerdict::Taken);
        // nothing above moved the row
        assert_eq!(p.live_owner(BASKET), C2);
        assert_eq!(p.epoch_of(BASKET), 1);
        // the rest returns it to the host: anyone's pickup is taken
        assert_eq!(p.sample(BASKET, C2, CELL, C1, true, 10.0, 603_000), TransformVerdict::Returned(2));
        assert_eq!(p.pickup(BASKET, C1, 603_100), PickupVerdict::Taken);
        assert_eq!(p.pickup(BASKET, C3, 603_100), PickupVerdict::Taken);
    }

    #[test]
    fn only_the_owner_moves_the_record_and_its_rest_returns_the_prop_to_the_host_as_a_word() {
        let mut p = Props::default();
        // the host's own sample of an unclaimed prop wakes it: the row the host's at epoch 1, the word naming the host
        // by id; its next sample is the live row's; its rest returns it with a word too (player 0, epoch 2)
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 500), TransformVerdict::Woken(1));
        assert_eq!(p.live_owner(BASKET), C1);
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 550), TransformVerdict::Accepted);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 10.0, 600), TransformVerdict::NotOwner);
        assert_eq!(p.sample(BASKET, C1, CELL, C1, true, 10.0, 700), TransformVerdict::Returned(2));
        assert_eq!(p.epoch_of(BASKET), 2);
        assert_eq!(p.owner_of(BASKET, C1), C1);
        // c2's touch at rest: granted; only c2's samples pass
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(3));
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 1_100), TransformVerdict::NotOwner);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 10.0, 1_100), TransformVerdict::Accepted);
        // no timer hands it back: a long quiet keeps c2 the owner, the host's own sample refused as any non-owner's
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 60_000), TransformVerdict::NotOwner);
        assert_eq!(p.owner_of(BASKET, C1), C2);
        // c2's own asleep sample: the host's again at once, at the next epoch, and the word goes out
        assert_eq!(p.sample(BASKET, C2, CELL, C1, true, 10.0, 61_000), TransformVerdict::Returned(4));
        assert_eq!(p.owner_of(BASKET, C1), C1);
        assert_eq!(p.live_owner(BASKET), 0);
        assert_eq!(p.epoch_of(BASKET), 4);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 10.0, 61_100), TransformVerdict::NotOwner);
        // the host's kick of the resting prop is a wake, by id (epoch 5); its rest returns it again (6); c2's new touch
        // at rest is a grant at 7
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 61_100), TransformVerdict::Woken(5));
        assert_eq!(p.live_owner(BASKET), C1);
        assert_eq!(p.sample(BASKET, C1, CELL, C1, true, 10.0, 61_200), TransformVerdict::Returned(6));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 62_000), ClaimVerdict::Granted(7));
    }

    #[test]
    fn a_prop_in_no_cell_or_an_unhosted_unclaimed_cell_freezes() {
        let mut p = Props::default();
        // no cell at the sample's point: refused whoever sends it (ADR-071 decision 6's conservation)
        assert_eq!(p.sample(BASKET, C1, 0, C1, false, 10.0, 0), TransformVerdict::Outside);
        // a resting prop in a cell nobody hosts: frozen at the record
        assert_eq!(p.sample(BASKET, C1, CELL, 0, false, 10.0, 0), TransformVerdict::Frozen);
        // a live toucher keeps simulating in a cell whose host left, never past the world's cells; its rest returns
        // the prop to nobody, frozen again
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.sample(BASKET, C1, CELL, 0, false, 10.0, 1_100), TransformVerdict::Accepted);
        assert_eq!(p.sample(BASKET, C1, 0, 0, false, 10.0, 1_150), TransformVerdict::Outside);
        assert_eq!(p.sample(BASKET, C1, CELL, 0, true, 10.0, 1_200), TransformVerdict::Returned(2));
        assert_eq!(p.sample(BASKET, C1, CELL, 0, false, 10.0, 1_300), TransformVerdict::Frozen);
    }

    #[test]
    fn a_host_change_changes_no_row_the_live_ones_stay_and_the_resting_ones_are_the_new_hosts() {
        // A7: a grant changes no row (the table has no entry for it): a resting row names nobody already, so the new
        // host simulates it by default; a live row stays its owner's until its rest
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.sample(BASKET, C2, CELL, C1, true, 10.0, 1_500), TransformVerdict::Returned(2));
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 1_600), ClaimVerdict::Granted(1));
        let rows = p.rows();
        assert!(rows.iter().any(|(refr, row)| *refr == BASKET && row.owner == 0 && row.epoch == 2));
        // c3 hosts the cell now: the basket is its, the cabbage still c2's, nothing written
        assert_eq!(p.owner_of(BASKET, C3), C3);
        assert_eq!(p.owner_of(0x54a44, C3), C2);
        assert_eq!(p.rows(), rows);
        assert_eq!(p.epoch_of(BASKET), 2);
        // the new host's sample of the cabbage is refused until c2's rest returns it, then its own wake takes it by id
        assert_eq!(p.sample(0x54a44, C3, CELL, C3, false, 10.0, 1_700), TransformVerdict::NotOwner);
        assert_eq!(p.sample(0x54a44, C2, CELL, C3, true, 10.0, 1_800), TransformVerdict::Returned(2));
        assert_eq!(p.sample(0x54a44, C3, CELL, C3, false, 10.0, 1_900), TransformVerdict::Woken(3));
    }

    #[test]
    fn a_players_unload_of_a_cell_returns_its_live_rows_there_with_a_word_each() {
        // the review of 2026-11-03 (D2; ADR-071's D-PH2 A): the old host's cell unloads mid-flight (a door, a
        // teleport); its 3D of the prop is gone and the rescan untracks it before any final sample, so the row would
        // stay live naming it and nobody would stream; the unload returns its rows there to the host by default with a
        // word each, as a departure does
        let mut p = Props::default();
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 1_000), TransformVerdict::Woken(1));
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::GRAB, 1_100), ClaimVerdict::Granted(1));
        // a resting row and another cell's row are not the unload's
        assert_eq!(p.claim(0x1234, C1, CELL, touch::CONTACT, 1_200), ClaimVerdict::Granted(1));
        assert_eq!(p.sample(0x1234, C1, CELL, C1, true, 10.0, 1_300), TransformVerdict::Returned(2));
        assert_eq!(p.claim(0x5678, C1, 0x9733, touch::CONTACT, 1_400), ClaimVerdict::Granted(1));
        assert_eq!(p.unloaded(C1, CELL), vec![(BASKET, 2)]);
        assert_eq!(p.owner_of(BASKET, C2), C2);
        assert_eq!(p.live_owner(BASKET), 0);
        assert_eq!(p.epoch_of(BASKET), 2);
        assert_eq!(p.live_owner(0x54a44), C2);
        assert_eq!(p.live_owner(0x5678), C1);
        // the unload is a state: until c1's game reports the cell loaded again (or the cell is in its reach again), its
        // grants and wakes there are refused (an unloading host still hosts through its grace, and its late sample would
        // wake the prop by id again; the same seat's claim would be granted again), while its rows elsewhere stand and a
        // live row of its own streams on wherever the sample lands (a missed load report never strands a stream)
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 2_050), TransformVerdict::Away);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 2_060), ClaimVerdict::Away);
        assert_eq!(p.sample(0x5678, C1, 0x9733, C1, false, 10.0, 2_070), TransformVerdict::Accepted);
        assert_eq!(p.sample(0x5678, C1, CELL, C1, false, 10.0, 2_075), TransformVerdict::Accepted);
        p.reached(C1, &[0x9733, CELL]);
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 2_080), TransformVerdict::Woken(3));
        assert_eq!(p.sample(BASKET, C1, CELL, C1, true, 10.0, 2_090), TransformVerdict::Returned(4));
        // the second unload returns the cabbage alone: its stream landed it in this cell at 2_075, the basket rests
        assert_eq!(p.unloaded(C1, CELL), vec![(0x5678, 2)]);
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 2_096), TransformVerdict::Away);
        p.loaded(C1, CELL);
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 2_097), TransformVerdict::Woken(5));
        assert_eq!(p.sample(BASKET, C1, CELL, C1, true, 10.0, 2_098), TransformVerdict::Returned(6));
        // the holder's unload drops its grab too (a grab carried through a door stays in no hand), and its claim
        // there waits for its load report as well; a departure clears the state
        assert_eq!(p.unloaded(C2, CELL), vec![(0x54a44, 2)]);
        assert_eq!(p.live_owner(0x54a44), 0);
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::GRAB, 2_150), ClaimVerdict::Away);
        assert_eq!(p.gone(C2), vec![]);
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 2_300), ClaimVerdict::Granted(3));
        // the live owner's own stream into a cell it reported unloaded passes (its row is live), a wake there does not
        assert_eq!(p.unloaded(C2, 0x9733), vec![]);
        assert_eq!(p.sample(0x54a44, C2, 0x9733, C1, false, 10.0, 2_410), TransformVerdict::Accepted);
        assert_eq!(p.sample(0x54a44, C2, 0x9733, C1, true, 10.0, 2_420), TransformVerdict::Returned(4));
        // nothing of c3's, and nothing twice
        assert_eq!(p.unloaded(C3, CELL), vec![]);
        assert_eq!(p.unloaded(C1, CELL), vec![]);
    }

    #[test]
    fn a_departed_players_live_rows_return_to_the_host_with_a_word_each() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 1_100), ClaimVerdict::Granted(1));
        // a resting row of c2's is the host's already: no word for it
        assert_eq!(p.claim(0x1234, C2, CELL, touch::CONTACT, 1_200), ClaimVerdict::Granted(1));
        assert_eq!(p.sample(0x1234, C2, CELL, C1, true, 10.0, 1_300), TransformVerdict::Returned(2));
        assert_eq!(p.gone(C2), vec![(BASKET, 2), (0x54a44, 2)]);
        assert_eq!(p.owner_of(BASKET, C1), C1);
        assert_eq!(p.owner_of(BASKET, 0), 0);
        assert_eq!(p.epoch_of(BASKET), 2);
        assert_eq!(p.epoch_of(0x1234), 2);
        // the host's samples pass and it streams the prop awake (its wake, by id, epoch 3): c2's return touch waits
        // for the host's rest (a word, epoch 4), then is a grant at 5
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 2_200), TransformVerdict::Woken(3));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 2_300), ClaimVerdict::Moving);
        assert_eq!(p.sample(BASKET, C1, CELL, C1, true, 10.0, 2_400), TransformVerdict::Returned(4));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 2_500), ClaimVerdict::Granted(5));
        // nothing of c3's: nothing returned
        assert_eq!(p.gone(C3), vec![]);
    }

    #[test]
    fn the_hosts_wake_word_names_it_by_id_so_a_host_change_leaves_it_streaming_until_its_rest() {
        // the review of 2026-11-03: the host's kick wrote the row with the host's id and no word, so every seat held
        // (0, E); at a host change the old host's client read itself as no owner and stopped without a final sample,
        // the new host's samples were refused and answered with the old host's name, and nobody streamed. The wake is
        // a word by id: the old host keeps streaming across the grant until its rest, which returns the prop to the
        // host by default, the new one
        let mut p = Props::default();
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 10.0, 1_000), TransformVerdict::Woken(1));
        assert_eq!(p.live_owner(BASKET), C1);
        // c2 hosts the cell from here
        assert_eq!(p.owner_of(BASKET, C2), C1);
        assert_eq!(p.sample(BASKET, C2, CELL, C2, false, 10.0, 1_200), TransformVerdict::NotOwner);
        assert_eq!(p.sample(BASKET, C1, CELL, C2, false, 10.0, 1_200), TransformVerdict::Accepted);
        assert_eq!(p.sample(BASKET, C1, CELL, C2, true, 10.0, 1_300), TransformVerdict::Returned(2));
        assert_eq!(p.owner_of(BASKET, C2), C2);
        assert_eq!(p.sample(BASKET, C2, CELL, C2, false, 10.0, 1_400), TransformVerdict::Woken(3));
        assert_eq!(p.sample(BASKET, C2, CELL, C2, true, 10.0, 1_500), TransformVerdict::Returned(4));
        // a non-host's sample never wakes a resting prop
        assert_eq!(p.sample(BASKET, C3, CELL, C2, false, 10.0, 1_600), TransformVerdict::NotOwner);
        assert_eq!(p.live_owner(BASKET), 0);
    }

    // thuum D-S3 point 4, under A1 to A7 (ADR-071 decision 8): Embershard Mine
    // 0xb6be6's falling rocks (0xb6d45 and its six siblings, thuum
    // docs/research/trap-census-2026-11-05.md row 18), claimed by the impulse
    // touch like any prop
    const MINE: u32 = 0xb6be6;
    const ROCK: u32 = 0xb6d45;

    #[test]
    fn a_trap_bodys_first_firing_wins_while_it_moves() {
        let mut p = Props::default();
        // c2's firing set the rock moving: its impulse claim is granted
        assert_eq!(p.claim(ROCK, C2, MINE, touch::IMPULSE, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.owner_of(ROCK, C1), C2);
        // c1's own copy of the same firing: refused while the body moves (A3), before and after c2's first awake
        // sample, the epoch unmoved
        assert_eq!(p.claim(ROCK, C1, MINE, touch::IMPULSE, 1_050), ClaimVerdict::Moving);
        assert_eq!(p.sample(ROCK, C2, MINE, C1, false, 40.0, 1_100), TransformVerdict::Accepted);
        assert_eq!(p.claim(ROCK, C1, MINE, touch::IMPULSE, 1_150), ClaimVerdict::Moving);
        assert_eq!(p.claim(ROCK, C3, MINE, touch::IMPULSE, 1_150), ClaimVerdict::Moving);
        assert_eq!(p.owner_of(ROCK, C1), C2);
        assert_eq!(p.epoch_of(ROCK), 1);
        // the firing seat's own claim again is Same
        assert_eq!(p.claim(ROCK, C2, MINE, touch::IMPULSE, 1_200), ClaimVerdict::Same);
        // the rest sample returns the rock to the host with the word (epoch 2), and a new firing takes the resting
        // body at the next epoch
        assert_eq!(p.sample(ROCK, C2, MINE, C1, true, 40.0, 1_300), TransformVerdict::Returned(2));
        assert_eq!(p.owner_of(ROCK, C1), C1);
        assert_eq!(p.claim(ROCK, C1, MINE, touch::IMPULSE, 1_400), ClaimVerdict::Granted(3));
        assert_eq!(p.owner_of(ROCK, C2), C1);
    }

    #[test]
    fn a_stalled_trap_bodys_row_is_any_firings() {
        let mut p = Props::default();
        assert_eq!(p.claim(ROCK, C2, MINE, touch::IMPULSE, 1_000), ClaimVerdict::Granted(1));
        // c2 never streams: past the moving window the row is stalled, and c1's firing takes the body
        assert_eq!(p.claim(ROCK, C1, MINE, touch::IMPULSE, 1_000 + PROP_MOVING_MS - 1), ClaimVerdict::Moving);
        assert_eq!(p.claim(ROCK, C1, MINE, touch::IMPULSE, 1_000 + PROP_MOVING_MS), ClaimVerdict::Granted(2));
    }

    #[test]
    fn an_impulse_never_takes_a_moving_prop_and_a_grab_does() {
        // A3 retires slice one's newest touch for items: a shout's impulse on a basket another seat kicked is refused
        // while it flies (the caster's claims are rung 3's, ADR-071 decision 4); a grab takes it
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 10.0, 1_100), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::IMPULSE, 1_150), ClaimVerdict::Moving);
        assert_eq!(p.owner_of(BASKET, C1), C2);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::GRAB, 1_200), ClaimVerdict::Granted(2));
        assert_eq!(p.owner_of(BASKET, C2), C1);
    }

    #[test]
    fn a_sample_farther_than_the_step_budget_is_refused_and_leaves_the_record() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        // the first sample after the grant: a new row's full allowance, half
        // a second at the cap for an epoch's first sample (physics rung 3a)
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, PROP_FIRST_SAMPLE_UNITS + 1.0, 1_010), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 2_900.0, 1_010), TransformVerdict::Accepted);
        // 100 units left, and 50 ms later 300 more at 6000 a second, a later
        // sample's allowance one step at most
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 401.0, 1_060), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 400.0, 1_060), TransformVerdict::Accepted);
        // a refused sample spends nothing and moves no clock: the next is
        // judged from the last accepted, 40 ms of refill
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 5_000.0, 1_100), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 241.0, 1_100), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 240.0, 1_100), TransformVerdict::Accepted);
        // a long pause refills one full step and never more: no banked
        // teleport after a silence
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS + 1.0, 2_900), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 2_900), TransformVerdict::Accepted);
        // not a number: refused
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, f32::NAN, 3_000), TransformVerdict::TooFast);
        // an asleep sample over the budget: clamped to what the budget held
        // (one full step, 200 ms after the last), the prop the host's again
        // with a word
        assert_eq!(p.sample(BASKET, C2, CELL, C1, true, 5_000.0, 3_100), TransformVerdict::Clamped { allowed: PROP_STEP_ALLOWANCE_UNITS, epoch: 2 });
        assert_eq!(p.owner_of(BASKET, C1), C1);
        assert_eq!(p.live_owner(BASKET), 0);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 10.0, 3_200), TransformVerdict::NotOwner);
        // the host's first sample of an unclaimed prop: a new row's full
        // allowance, which the step spends; the first is its wake, and what
        // it leaves is one step at most, so the woken epoch's first sample at
        // the same moment holds that step and no more (the epoch rise buys no
        // time, physics rung 3a)
        let mut q = Props::default();
        assert_eq!(q.sample(0x54a44, C1, CELL, C1, false, PROP_FIRST_SAMPLE_UNITS + 1.0, 0), TransformVerdict::TooFast);
        assert_eq!(q.sample(0x54a44, C1, CELL, C1, false, 200.0, 0), TransformVerdict::Woken(1));
        assert_eq!(q.sample(0x54a44, C1, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS + 1.0, 0), TransformVerdict::TooFast);
        assert_eq!(q.sample(0x54a44, C1, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 0), TransformVerdict::Accepted);
    }

    #[test]
    #[allow(clippy::arithmetic_side_effects)] // a test's clock, never a packet's path
    fn a_crafted_96_hz_stream_is_held_to_the_speed_cap() {
        // the review of 9ff1895e (finding 3): at the prop budget's 96 samples
        // a second, a per-sample floor of 128 units let one crafted owner
        // carry a prop 12,288 units a second, twice the cap. The allowance
        // holds the record to the cap over any window, plus its one step
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        let mut carried = 0.0_f32;
        let mut refused = 0_u32;
        // ten seconds of 128-unit steps, 96 a second
        for i in 1..=960_u64 {
            let verdict = p.sample(BASKET, C2, CELL, C1, false, 128.0, 1_000 + i * 1_000 / 96);
            assert!(verdict == TransformVerdict::Accepted || verdict == TransformVerdict::TooFast, "{verdict:?}");
            if verdict == TransformVerdict::Accepted {
                carried += 128.0;
            } else {
                refused += 1;
            }
        }
        // refused past the cap: ten seconds of it and the one step at most
        assert!(carried <= PROP_MAX_SPEED_UNITS_PER_S * 10.0 + PROP_STEP_ALLOWANCE_UNITS, "carried {carried}");
        assert!(refused > 0);
        // and the cap, no floor, is the limit: the stream still gets the
        // cap's worth, less one sample's step
        assert!(carried >= PROP_MAX_SPEED_UNITS_PER_S * 10.0 - 128.0, "carried {carried}");
    }

    #[test]
    #[allow(clippy::arithmetic_side_effects)] // a test's clock, never a packet's path
    fn an_honest_stream_under_the_cap_is_taken_whole() {
        // the client's cadence, one sample per awake body per 100 ms: a body
        // at the speed cap (600 units a step) for ten seconds, every step
        // taken
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        for i in 1..=100_u64 {
            assert_eq!(p.sample(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 1_000 + i * 100), TransformVerdict::Accepted);
        }
        // a kick at the engine's 7 m/s (476 units a second, thuum docs/verbs/
        // prop-physics.md's x-prop-launch v3 read): 47.6 a step at 10 Hz, and
        // 5 a step at the prop budget's 96 a second
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        for i in 1..=100_u64 {
            assert_eq!(q.sample(BASKET, C2, CELL, C1, false, 47.6, 1_000 + i * 100), TransformVerdict::Accepted);
        }
        for i in 1..=960_u64 {
            assert_eq!(q.sample(BASKET, C2, CELL, C1, false, 5.0, 11_000 + i * 1_000 / 96), TransformVerdict::Accepted);
        }
        // the prop budget's whole 96 samples a second spent on one trap body
        // falling at 4,800 units a second (50 a step): taken whole, since
        // the allowance judges distance, never the rate
        let mut r = Props::default();
        assert_eq!(r.claim(ROCK, C2, MINE, touch::IMPULSE, 1_000), ClaimVerdict::Granted(1));
        for i in 1..=960_u64 {
            assert_eq!(r.sample(ROCK, C2, MINE, C1, false, 50.0, 1_000 + i * 1_000 / 96), TransformVerdict::Accepted);
        }
    }

    #[test]
    fn a_grant_leaves_the_rows_allowance_as_it_stands() {
        // the allowance is the prop's, refilled by time alone: seats trading
        // a prop by claims cannot each start with a full one (c3's grab takes
        // the moving prop, A2, the allowance as c2 left it). c2's first sample
        // spends the whole of a new row's, and c3's first after its grant may
        // spend up to half a second of the prop's clock (physics rung 3a), of
        // which none has run
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, PROP_FIRST_SAMPLE_UNITS, 1_000), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C3, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(2));
        assert_eq!(p.sample(BASKET, C3, CELL, C1, false, 1.0, 1_000), TransformVerdict::TooFast);
        // 50 ms later, 300 units refilled at 6000 a second
        assert_eq!(p.sample(BASKET, C3, CELL, C1, false, 301.0, 1_050), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C3, CELL, C1, false, 300.0, 1_050), TransformVerdict::Accepted);
    }

    #[test]
    fn a_sample_is_judged_by_its_epoch_first() {
        // ADR-066 decision 4: by its epoch first, then by its owner table,
        // never by its position. A granted row at 2: c1's contact at 1, c2's
        // grab of the moving prop at 2
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::GRAB, 1_100), ClaimVerdict::Granted(2));
        let rows = p.rows();
        // the owner's samples at 1 and 3 are Stale and Future
        assert_eq!(p.transform(&facts(BASKET, C2, 1, 1, 1_200, CELL, C1, false, true, 10.0, 1_200)), TransformVerdict::Stale);
        assert_eq!(p.transform(&facts(BASKET, C2, 3, 1, 1_200, CELL, C1, false, true, 10.0, 1_200)), TransformVerdict::Future);
        // c1's at 1, its own old grant's, is Stale before it is anyone's;
        // its sample at 2 is NotOwner; c3's at 3 is Future
        assert_eq!(p.transform(&facts(BASKET, C1, 1, 9, 1_200, CELL, C1, false, false, 10.0, 1_200)), TransformVerdict::Stale);
        assert_eq!(p.transform(&facts(BASKET, C1, 2, 1, 1_200, CELL, C1, false, false, 10.0, 1_200)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C3, 3, 1, 1_200, CELL, C1, false, false, 10.0, 1_200)), TransformVerdict::Future);
        // conservation comes before any epoch: no cell, whatever it names
        assert_eq!(p.transform(&facts(BASKET, C2, 7, 1, 1_200, 0, C1, false, true, 10.0, 1_200)), TransformVerdict::Outside);
        // none moved the record or spent the bucket: the rows stand, and the
        // owner's sample at 2 takes the whole step
        assert_eq!(p.rows(), rows);
        assert_eq!(p.transform(&facts(BASKET, C2, 2, 1, 1_200, CELL, C1, false, true, PROP_STEP_ALLOWANCE_UNITS, 1_200)), TransformVerdict::Accepted);
        // the woken host's sample in flight (no grace, ADR-066 decision 5 not
        // built): its wake raised the epoch, and a sample it sent before the
        // wake's word reached it names the old one
        let mut q = Props::default();
        assert_eq!(q.transform(&facts(BASKET, C1, 0, 1, 500, CELL, C1, false, false, 10.0, 500)), TransformVerdict::Woken(1));
        assert_eq!(q.transform(&facts(BASKET, C1, 0, 2, 600, CELL, C1, false, false, 10.0, 600)), TransformVerdict::Stale);
        assert_eq!(q.transform(&facts(BASKET, C1, 1, 1, 700, CELL, C1, false, false, 10.0, 700)), TransformVerdict::Accepted);
    }

    #[test]
    fn a_late_or_repeated_seq_spends_nothing() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        let at = |seq: u16, moved: f32, now_ms: u64| facts(BASKET, C2, 1, seq, u32::try_from(now_ms).unwrap_or(0), CELL, C1, false, false, moved, now_ms);
        // a sender's first at an epoch may be any sequence but 0
        assert_eq!(p.transform(&at(5, 10.0, 1_100)), TransformVerdict::Accepted);
        // 5 again and 4 are Late, and spend nothing: the row stands
        let rows = p.rows();
        assert_eq!(p.transform(&at(5, 590.0, 1_200)), TransformVerdict::Late);
        assert_eq!(p.transform(&at(4, 590.0, 1_200)), TransformVerdict::Late);
        assert_eq!(p.rows(), rows);
        // 6 is newer and takes the whole step
        assert_eq!(p.transform(&at(6, PROP_STEP_ALLOWANCE_UNITS, 1_200)), TransformVerdict::Accepted);
        // across the wrap the sender skips 0: 65534, 65535, 1 each newer,
        // 65535 again behind 1, and 0 (the record answer's mark) never a
        // sample
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        let wrap = |seq: u16, now_ms: u64| facts(BASKET, C2, 1, seq, u32::try_from(now_ms).unwrap_or(0), CELL, C1, false, false, 10.0, now_ms);
        assert_eq!(q.transform(&wrap(65_534, 1_100)), TransformVerdict::Accepted);
        assert_eq!(q.transform(&wrap(65_535, 1_200)), TransformVerdict::Accepted);
        assert_eq!(q.transform(&wrap(1, 1_300)), TransformVerdict::Accepted);
        assert_eq!(q.transform(&wrap(65_535, 1_400)), TransformVerdict::Late);
        assert_eq!(q.transform(&wrap(0, 1_500)), TransformVerdict::Late);
        // the last sequence is the epoch's: after the rest's return and the
        // next grant a seat starts again from 1, on its own clock
        assert_eq!(q.transform(&facts(BASKET, C2, 1, 2, 1_600, CELL, C1, true, false, 10.0, 1_600)), TransformVerdict::Returned(2));
        assert_eq!(q.claim(BASKET, C3, CELL, touch::CONTACT, 1_700), ClaimVerdict::Granted(3));
        assert_eq!(q.transform(&facts(BASKET, C3, 3, 1, 50, CELL, C1, false, false, 10.0, 1_800)), TransformVerdict::Accepted);
    }

    #[test]
    #[allow(clippy::arithmetic_side_effects)] // a test's clocks, never a packet's path
    fn the_owners_clock_takes_a_bunched_honest_stream() {
        // ten samples 100 ms apart on the owner's clock, each one step at the
        // cap (600 units), reach the server 5 ms apart after a stall of a
        // second: the owner's clock refills each, and all are taken
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C2, 1, 1, 0, CELL, C1, false, false, PROP_STEP_ALLOWANCE_UNITS, 1_000)), TransformVerdict::Accepted);
        for i in 1..=10_u16 {
            let (t_ms, now_ms) = (u32::from(i) * 100, 2_000 + u64::from(i) * 5);
            assert_eq!(p.transform(&facts(BASKET, C2, 1, i + 1, t_ms, CELL, C1, false, false, PROP_STEP_ALLOWANCE_UNITS, now_ms)), TransformVerdict::Accepted, "sample {i}");
        }
        // the regression's witness: the same arrivals judged by the server's
        // clock alone (the sender's clock read as the server's, rung 1's
        // refill) take the first and refuse every one after it
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.transform(&facts(BASKET, C2, 1, 1, 1_000, CELL, C1, false, false, PROP_STEP_ALLOWANCE_UNITS, 1_000)), TransformVerdict::Accepted);
        for i in 1..=10_u16 {
            let now_ms = 2_000 + u64::from(i) * 5;
            let want = if i == 1 { TransformVerdict::Accepted } else { TransformVerdict::TooFast };
            assert_eq!(q.transform(&facts(BASKET, C2, 1, i + 1, u32::try_from(now_ms).unwrap_or(0), CELL, C1, false, false, PROP_STEP_ALLOWANCE_UNITS, now_ms)), want, "sample {i}");
        }
    }

    #[test]
    #[allow(clippy::arithmetic_side_effects)] // a test's clocks, never a packet's path
    fn a_crafted_clock_buys_no_more_than_the_servers_time_and_its_slack() {
        // an owner's clock running ten times the server's: samples 10 ms
        // apart on the server's clock each claim 100 ms and a whole step
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        let mut carried = 0.0_f32;
        for i in 1..=300_u16 {
            let (t_ms, now_ms) = (u32::from(i) * 100, 1_000 + u64::from(i) * 10);
            if p.transform(&facts(BASKET, C2, 1, i, t_ms, CELL, C1, false, false, PROP_STEP_ALLOWANCE_UNITS, now_ms)) == TransformVerdict::Accepted {
                carried += PROP_STEP_ALLOWANCE_UNITS;
            }
        }
        // three seconds of the server's: the cap's pace for them and the
        // slack, plus the one step, at most; and the cap's pace at least
        let slack_s = f32::from(u16::try_from(PROP_CLOCK_SLACK_MS).unwrap_or(0)) / 1_000.0;
        let bound = PROP_MAX_SPEED_UNITS_PER_S * (3.0 + slack_s) + PROP_STEP_ALLOWANCE_UNITS;
        assert!(carried <= bound, "carried {carried} past {bound}");
        assert!(carried >= PROP_MAX_SPEED_UNITS_PER_S * 3.0 - PROP_STEP_ALLOWANCE_UNITS, "carried {carried}");
    }

    #[test]
    #[allow(clippy::arithmetic_side_effects)] // a test's clocks, never a packet's path
    fn a_still_clock_banks_no_more_than_the_lag_and_the_slack() {
        // a credit counted from the epoch's first sample (the build plan's
        // first form) banked the whole epoch: an owner whose clock stood
        // still for a minute while it sampled could then spend the minute at
        // a full step a sample, 60,000 units in a second. The budget's clock
        // falls no more than PROP_CLOCK_LAG_MS behind the server's, so the
        // owner buys back the lag and the slack, then the server's pace
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        // a minute at 10 Hz, the owner's clock at 0 throughout, nothing moved
        for i in 1..=600_u16 {
            assert_eq!(p.transform(&facts(BASKET, C2, 1, i, 0, CELL, C1, false, false, 0.0, 1_000 + u64::from(i) * 100)), TransformVerdict::Accepted);
        }
        // then a whole step at every sample, 10 ms apart on the server's
        // clock, 100 ms on the owner's
        let mut carried = 0.0_f32;
        for i in 1..=100_u16 {
            let (t_ms, now_ms) = (u32::from(i) * 100, 61_000 + u64::from(i) * 10);
            if p.transform(&facts(BASKET, C2, 1, 600 + i, t_ms, CELL, C1, false, false, PROP_STEP_ALLOWANCE_UNITS, now_ms)) == TransformVerdict::Accepted {
                carried += PROP_STEP_ALLOWANCE_UNITS;
            }
        }
        // one second of the server's: its pace, the lag and the slack, and
        // the one step, at most
        let lag_s = f32::from(u16::try_from(PROP_CLOCK_LAG_MS).unwrap_or(0)) / 1_000.0;
        let slack_s = f32::from(u16::try_from(PROP_CLOCK_SLACK_MS).unwrap_or(0)) / 1_000.0;
        let bound = PROP_MAX_SPEED_UNITS_PER_S * (1.0 + lag_s + slack_s) + PROP_STEP_ALLOWANCE_UNITS;
        assert!(carried <= bound, "carried {carried} past {bound}");
    }

    #[test]
    #[allow(clippy::arithmetic_side_effects)] // a test's clocks, never a packet's path
    fn an_epoch_rise_starts_no_fresh_slack() {
        // the budget's clock is the prop's: a host that wakes its prop, takes
        // a step on a crafted clock and rests it, over and over, gets no new
        // slack at each epoch (one begun at each epoch bought a whole step
        // every cycle, 15,000 units a second here)
        let mut p = Props::default();
        let mut carried = 0.0_f32;
        let (mut now_ms, mut t_ms) = (1_000_u64, 0_u32);
        for _ in 0..100 {
            // the wake, the new epoch's first sample, its crafted step, its rest
            for (asleep, moved) in [(false, 0.0), (false, 0.0), (false, PROP_STEP_ALLOWANCE_UNITS), (true, 0.0)] {
                now_ms += 10;
                t_ms += 1_000;
                let epoch = p.epoch_of(BASKET);
                let seq = p.rows.get(&BASKET).map_or(1, |r| r.last_seq.checked_add(1).unwrap_or(1));
                let verdict = p.transform(&facts(BASKET, C1, epoch, seq, t_ms, CELL, C1, asleep, false, moved, now_ms));
                if matches!(verdict, TransformVerdict::Accepted | TransformVerdict::Woken(_) | TransformVerdict::Returned(_)) {
                    carried += moved;
                }
            }
        }
        // four seconds of the server's: the cap's pace, the one slack and the
        // one step, at most
        let elapsed_s = f32::from(u16::try_from(now_ms - 1_000).unwrap_or(u16::MAX)) / 1_000.0;
        let slack_s = f32::from(u16::try_from(PROP_CLOCK_SLACK_MS).unwrap_or(0)) / 1_000.0;
        let bound = PROP_MAX_SPEED_UNITS_PER_S * (elapsed_s + slack_s) + PROP_STEP_ALLOWANCE_UNITS;
        assert!(carried <= bound, "carried {carried} past {bound}");
        assert_eq!(p.epoch_of(BASKET), 200);
    }

    #[test]
    fn a_rest_past_the_bucket_is_clamped_and_returns_the_prop() {
        // ADR-071 decision 6: a live owner's rest past the bucket is clamped
        // and accepted. An asleep sample 3,500 units off right after the
        // grant (past the first sample's half second at the cap, physics rung
        // 3a; 1,500 before it): the record moves the whole first allowance
        // toward it, the prop is the host's again at 2
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(
            p.transform(&facts(BASKET, C2, 1, 1, 50, CELL, C1, true, false, 3_500.0, 1_010)),
            TransformVerdict::Clamped { allowed: PROP_FIRST_SAMPLE_UNITS, epoch: 2 }
        );
        assert_eq!(p.live_owner(BASKET), 0);
        assert_eq!(p.owner_of(BASKET, C1), C1);
        assert_eq!(p.epoch_of(BASKET), 2);
        // a sample after it at the old epoch is Stale
        assert_eq!(p.transform(&facts(BASKET, C2, 1, 2, 150, CELL, C1, false, false, 10.0, 1_100)), TransformVerdict::Stale);
        // the clamp spent the budget: the host's wake 10 ms after it holds 60
        // units
        assert_eq!(p.transform(&facts(BASKET, C1, 2, 1, 1_020, CELL, C1, false, false, 61.0, 1_020)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C1, 2, 1, 1_020, CELL, C1, false, false, 60.0, 1_020)), TransformVerdict::Woken(3));
        // a rest whose distance is not a number takes the rest and moves
        // the record nothing; a holder's rest past the bucket ends its hold
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.transform(&facts(BASKET, C2, 1, 1, 0, CELL, C1, true, false, f32::NAN, 1_010)), TransformVerdict::Clamped { allowed: 0.0, epoch: 2 });
        assert_eq!(q.claim(0x54a44, C3, CELL, touch::GRAB, 1_100), ClaimVerdict::Granted(1));
        assert!(q.held(0x54a44));
        assert_eq!(q.transform(&facts(0x54a44, C3, 1, 1, 0, CELL, C1, true, false, 5_000.0, 1_110)), TransformVerdict::Clamped { allowed: PROP_FIRST_SAMPLE_UNITS, epoch: 2 });
        assert!(!q.held(0x54a44));
        assert_eq!(q.live_owner(0x54a44), 0);
    }

    #[test]
    fn a_blasted_props_first_sample_after_a_grant_has_half_a_second_of_flight() {
        // physics rung 3a (thuum lab/.cache/drafts/physics/rung3-plan.md A2): a body an impulse moves flies before the
        // word naming its mover reaches that seat. The cart's cabbage 0x1c0c6 left at 1,353.7 units a second under
        // Unrelenting Force (20261111-223256 on c1): 0.6 s from the impact to the caster's first read (a lost grant
        // waits for renet's 300 ms resend) is 812 units, past one step, which refused that first sample and every
        // later one while the body flew on, until the rest's clamp
        const CABBAGE: u32 = 0x1c0c6;
        let mut p = Props::default();
        // the cabbage rests on the cart: the host's kick and rest of it, a minute before the cast
        assert_eq!(p.sample(CABBAGE, C1, CELL, C1, false, 10.0, 1_000), TransformVerdict::Woken(1));
        assert_eq!(p.sample(CABBAGE, C1, CELL, C1, true, 10.0, 1_100), TransformVerdict::Returned(2));
        // c2's impact claims it at rest (granted at the next epoch), and c2's first sample 0.6 s on stands 812 off:
        // taken, and the stream goes on at the launch's 135 units a step
        assert_eq!(p.claim(CABBAGE, C2, CELL, touch::IMPULSE, 60_000), ClaimVerdict::Granted(3));
        assert_eq!(p.sample(CABBAGE, C2, CELL, C1, false, 812.0, 60_600), TransformVerdict::Accepted);
        for i in 1..=10_u64 {
            assert_eq!(p.sample(CABBAGE, C2, CELL, C1, false, 135.4, 60_600 + i * 100), TransformVerdict::Accepted, "step {i}");
        }
        // at once after a grant, half a second at the cap: a sample 2,500 units off is taken, one more 600 units off at
        // the same moment is past what it left (500: the first allowance's rest, a step at most), 500 is taken
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::IMPULSE, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.sample(BASKET, C2, CELL, C1, false, 2_500.0, 1_000), TransformVerdict::Accepted);
        assert_eq!(q.sample(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 1_000), TransformVerdict::TooFast);
        assert_eq!(q.sample(BASKET, C2, CELL, C1, false, 500.0, 1_000), TransformVerdict::Accepted);
        // a host's wake of a body its own engine moved far before its client read it (rung 1's R12: the rescan's
        // late first read): the wake is the returned epoch's first sample
        let mut r = Props::default();
        assert_eq!(r.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(r.sample(BASKET, C2, CELL, C1, true, 10.0, 1_100), TransformVerdict::Returned(2));
        assert_eq!(r.sample(BASKET, C1, CELL, C1, false, 1_500.0, 30_000), TransformVerdict::Woken(3));
    }

    #[test]
    fn a_first_samples_allowance_is_one_an_epoch_and_never_banked() {
        // physics rung 3a: the epoch's first sample alone may spend the half second; a refused one keeps the mark (no
        // sample yet at the epoch); a later epoch's first is judged by the half second again and never more; and an
        // epoch rise buys no time the prop's clock has not run
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::IMPULSE, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, PROP_FIRST_SAMPLE_UNITS + 1.0, 1_000), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, 2_000.0, 1_000), TransformVerdict::Accepted);
        // the epoch's second sample is judged by one step: what the first left, a step at most, and 100 ms of refill
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS + 1.0, 1_100), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 1_100), TransformVerdict::Accepted);
        // a long quiet refills a later sample one step and never the half second
        assert_eq!(p.sample(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS + 1.0, 9_000), TransformVerdict::TooFast);
        // the rest returns it; c3's impact a second later takes it at a new epoch, its first sample judged by the half
        // second again: 6,000 units is refused, the half second taken
        assert_eq!(p.sample(BASKET, C2, CELL, C1, true, 10.0, 9_000), TransformVerdict::Returned(2));
        assert_eq!(p.claim(BASKET, C3, CELL, touch::IMPULSE, 10_000), ClaimVerdict::Granted(3));
        assert_eq!(p.sample(BASKET, C3, CELL, C1, false, 6_000.0, 10_000), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C3, CELL, C1, false, PROP_FIRST_SAMPLE_UNITS, 10_000), TransformVerdict::Accepted);
        // c1's grab at once after c3's first sample spent it all: c1's first sample holds nothing, and 100 ms on one
        // step's worth (the clock's 100 ms), not the half second
        assert_eq!(p.claim(BASKET, C1, CELL, touch::GRAB, 10_000), ClaimVerdict::Granted(4));
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, 1.0, 10_000), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS + 1.0, 10_100), TransformVerdict::TooFast);
        assert_eq!(p.sample(BASKET, C1, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 10_100), TransformVerdict::Accepted);
    }

    #[test]
    #[allow(clippy::arithmetic_side_effects)] // a test's clocks, never a packet's path
    fn a_wake_and_rest_cycle_buys_no_first_allowance_the_clock_has_not_run() {
        // physics rung 3a on rung 2's one clock: an epoch's first sample may spend up to half a second of the prop's
        // clock, never a fresh half second of its own. A refill at every epoch rise (the build plan's form) hands a
        // host that wakes and rests its prop 3,000 units at each rise, two rises a cycle, as fast as the prop budget's
        // 96 samples a second allow (about 190,000 units a second; a wake and a return are no claims, so the claim
        // budget bounds nothing). The crafted host asks the whole half second at every epoch's first sample, its
        // clock a hundred times the server's, for three seconds
        let mut p = Props::default();
        let mut carried = 0.0_f32;
        let (mut now_ms, mut t_ms) = (1_000_u64, 0_u32);
        for _ in 0..300 {
            now_ms += 10;
            t_ms += 1_000;
            let epoch = p.epoch_of(BASKET);
            let row = p.rows.get(&BASKET).copied();
            let seq = row.map_or(1, |r| r.last_seq.checked_add(1).unwrap_or(1));
            // no sample yet at the epoch (resting: the wake; woken: its first): the half second; else the rest
            let (asleep, moved) = if row.is_none_or(|r| r.last_seq == 0) { (false, PROP_FIRST_SAMPLE_UNITS) } else { (true, 0.0) };
            match p.transform(&facts(BASKET, C1, epoch, seq, t_ms, CELL, C1, asleep, false, moved, now_ms)) {
                TransformVerdict::Accepted | TransformVerdict::Woken(_) | TransformVerdict::Returned(_) => carried += moved,
                TransformVerdict::Clamped { allowed, .. } => carried += allowed,
                _ => {}
            }
        }
        // three seconds of the server's: the cap's pace, the slack and one half second, at most; the half second is
        // real (the cycle ran, its first samples taken once the clock had run their time)
        let elapsed_s = f32::from(u16::try_from(now_ms - 1_000).unwrap_or(u16::MAX)) / 1_000.0;
        let slack_s = f32::from(u16::try_from(PROP_CLOCK_SLACK_MS).unwrap_or(0)) / 1_000.0;
        let bound = PROP_MAX_SPEED_UNITS_PER_S * (elapsed_s + slack_s) + PROP_FIRST_SAMPLE_UNITS;
        assert!(carried <= bound, "carried {carried} past {bound}");
        assert!(carried >= PROP_FIRST_SAMPLE_UNITS * 4.0, "carried {carried}");
        assert!(p.epoch_of(BASKET) >= 4, "epoch {}", p.epoch_of(BASKET));
    }

    #[test]
    fn the_samples_held_flag_holds_and_ends_the_grab() {
        // Eli's D-PH3 A on the wire: an accepted sample sets the row's grab
        // to its engine's own
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        // c2's engine grabs the body it streams: its held sample makes another
        // seat's claim Held, a grab's included
        assert_eq!(p.transform(&facts(BASKET, C2, 1, 1, 100, CELL, C1, false, true, 10.0, 1_100)), TransformVerdict::Accepted);
        assert!(p.held(BASKET));
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_200), ClaimVerdict::Held);
        assert_eq!(p.claim(BASKET, C3, CELL, touch::GRAB, 1_200), ClaimVerdict::Held);
        // the first held-false sample ends the hold, no release claim needed:
        // the row moves on under c2 (a contact is Moving, A3)
        assert_eq!(p.transform(&facts(BASKET, C2, 1, 2, 200, CELL, C1, false, false, 10.0, 1_300)), TransformVerdict::Accepted);
        assert!(!p.held(BASKET));
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_400), ClaimVerdict::Moving);
        assert_eq!(p.live_owner(BASKET), C2);
        // its asleep sample returns it
        assert_eq!(p.transform(&facts(BASKET, C2, 1, 3, 300, CELL, C1, true, false, 10.0, 1_500)), TransformVerdict::Returned(2));
        assert_eq!(p.live_owner(BASKET), 0);
        // the host's grab of the resting prop wakes it held: the wake's word
        // names the host, the hold is its engine's
        assert_eq!(p.transform(&facts(BASKET, C1, 2, 1, 7_000, CELL, C1, false, true, 10.0, 1_600)), TransformVerdict::Woken(3));
        assert!(p.held(BASKET));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::GRAB, 1_700), ClaimVerdict::Held);
        // one hand per player: c1's grab of another prop moves the hand, and
        // a late held sample of the first never takes it back
        assert_eq!(p.claim(0x54a44, C1, CELL, touch::GRAB, 1_800), ClaimVerdict::Granted(1));
        assert!(!p.held(BASKET));
        assert_eq!(p.transform(&facts(BASKET, C1, 3, 1, 7_100, CELL, C1, false, true, 10.0, 1_900)), TransformVerdict::Accepted);
        assert!(!p.held(BASKET));
        assert!(p.held(0x54a44));
    }

    proptest! {
        #[test]
        fn no_owners_stream_carries_a_prop_past_the_cap(
            // (what: 0 to 2 a sample by the seat that streams the prop, the
            // host's wake while it rests; 3 the other seat's grab, a grant at a
            // new epoch; 4 the streamer's rest, a return at one; the
            // sequence's step, 0 a repeat; the streamer's clock's step, back
            // or ahead; the server's gap, under or past the client's 100 ms;
            // the step, under one or past an epoch's first allowance)
            steps in prop::collection::vec((0_u8..5, 0_u16..3, -500_i64..2_000, prop_oneof![0_u64..10, 0_u64..250], prop_oneof![0.0_f32..900.0, 0.0_f32..3_500.0]), 1..300),
        ) {
            // whatever three seats stream, at any pace, with any step, any
            // sequence and any clock (bunched, crafted ahead, run backwards),
            // trading the prop by grabs, rests and the host's wakes (each a new
            // epoch, whose first sample may spend half a second of the prop's
            // clock, physics rung 3a), the record moves no farther than the
            // cap allows for the server's elapsed time and the clock's slack,
            // plus one first sample's allowance, once however often the epoch
            // rises (the build plan's refill at each rise gained it at each)
            let seats = [C1, C2, C3];
            let mut p = Props::default();
            prop_assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
            let mut now = 1_000_u64;
            // each seat's own sequence and clock
            let (mut seqs, mut clocks) = ([0_u16; 3], [0_i64; 3]);
            let mut carried = 0.0_f64;
            let slack_s = f64::from(u32::try_from(PROP_CLOCK_SLACK_MS).unwrap_or(0)) / 1_000.0;
            for (what, seq_step, t_step, gap, moved) in steps {
                now = now.saturating_add(gap);
                // the seat that streams the prop: its live owner, else the
                // cell's host (c1), whose awake sample is a wake
                let live = p.live_owner(BASKET);
                let streamer = if live == 0 { C1 } else { live };
                if what == 3 {
                    // a grab takes a moving prop (A2) unless another seat holds it
                    let grabber = if streamer == C2 { C3 } else { C2 };
                    let _ = p.claim(BASKET, grabber, CELL, touch::GRAB, now);
                    continue;
                }
                let at = seats.iter().position(|&s| s == streamer).unwrap_or(0);
                let seq = seqs.get_mut(at).ok_or_else(|| TestCaseError::fail("no seat"))?;
                *seq = seq.wrapping_add(seq_step);
                let clock = clocks.get_mut(at).ok_or_else(|| TestCaseError::fail("no seat"))?;
                *clock = clock.saturating_add(t_step).clamp(0, i64::from(u32::MAX));
                let t_ms = u32::try_from(*clock).unwrap_or(u32::MAX);
                let epoch = p.epoch_of(BASKET);
                let step = match p.transform(&facts(BASKET, streamer, epoch, *seq, t_ms, CELL, C1, what == 4, false, moved, now)) {
                    TransformVerdict::Accepted | TransformVerdict::Woken(_) | TransformVerdict::Returned(_) => moved,
                    TransformVerdict::Clamped { allowed, .. } => allowed,
                    _ => 0.0,
                };
                carried += f64::from(step);
                let elapsed_s = f64::from(u32::try_from(now.saturating_sub(1_000)).unwrap_or(u32::MAX)) / 1_000.0;
                let bound = f64::from(PROP_MAX_SPEED_UNITS_PER_S) * (elapsed_s + slack_s) + f64::from(PROP_FIRST_SAMPLE_UNITS);
                prop_assert!(carried <= bound + 0.5, "carried {} past {} at {} ms", carried, bound, now);
            }
        }

        #[test]
        fn three_seats_keep_one_owner_and_every_word_raises_the_epoch(
            // (what, which seat, the touch, asleep (for an unload: unload or
            // load), the step, the gap before it, the sample's epoch against
            // the prop's (0 one below, 1 the prop's, 2 one above), its
            // sequence's step (0 a repeat), its hold)
            ops in prop::collection::vec((0_u8..6, 0_usize..3, 0_u8..4, any::<bool>(), 0.0_f32..50.0, 0_u64..700, 0_u32..3, 0_u16..3, any::<bool>()), 1..200),
        ) {
            // A1 to A7 over any interleaving of three seats' claims, samples,
            // departures and unloads, c1 hosting the cell: the record moves for
            // the owner alone and only at the prop's epoch; every word raises
            // the epoch by one, nothing else moves it, and it never falls; a
            // repeated or overtaken sequence is Late; a live row names its
            // owner whoever hosts; and no contact, drop or impulse takes a row
            // another seat holds or streams inside its moving window
            let seats = [C1, C2, C3];
            let mut p = Props::default();
            let mut now = 1_000_u64;
            // the stream as the words left it: its owner, its last accepted
            // sample (or the grant's or the claim's time) and its grab mark
            let mut stream: Option<(u32, u64, bool)> = None;
            // the last accepted sequence at the prop's epoch (0 before any),
            // and each seat's own counter
            let mut last_seq = 0_u16;
            let mut sent = [0_u16; 3];
            for (what, seat, touch, asleep, moved, gap, epoch_at, seq_step, held) in ops {
                now = now.saturating_add(gap);
                let player = seats.get(seat).copied().unwrap_or(C1);
                let epoch = p.epoch_of(BASKET);
                let owner = p.owner_of(BASKET, C1);
                let raised = epoch.saturating_add(1);
                match what {
                    0 | 1 => match p.claim(BASKET, player, CELL, touch, now) {
                        ClaimVerdict::Granted(e) => {
                            prop_assert_eq!(e, raised);
                            if let Some((streamer, last, held)) = stream {
                                prop_assert!(streamer != player, "a grant to the streaming seat itself");
                                prop_assert!(!held, "a grant over {:x}'s hold", streamer);
                                prop_assert!(touch == touch::GRAB || now >= last.saturating_add(PROP_MOVING_MS), "touch {} took {:x}'s moving prop", touch, streamer);
                            }
                            stream = Some((player, now, touch == touch::GRAB));
                            last_seq = 0;
                        }
                        ClaimVerdict::Same => {
                            prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            prop_assert_eq!(stream.map(|s| s.0), Some(player));
                            stream = Some((player, now, touch == touch::GRAB));
                        }
                        ClaimVerdict::Held => {
                            prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            prop_assert!(matches!(stream, Some((s, _, true)) if s != player));
                        }
                        ClaimVerdict::Moving => {
                            prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            prop_assert!(touch != touch::GRAB);
                            prop_assert!(matches!(stream, Some((s, last, false)) if s != player && now < last.saturating_add(PROP_MOVING_MS)));
                        }
                        ClaimVerdict::Away => prop_assert_eq!(p.epoch_of(BASKET), epoch),
                    },
                    2 | 3 => {
                        let worded = epoch.saturating_add(epoch_at).saturating_sub(1);
                        let counter = sent.get_mut(seat).ok_or_else(|| TestCaseError::fail("no seat"))?;
                        *counter = counter.wrapping_add(seq_step);
                        let seq = *counter;
                        let t_ms = u32::try_from(now).unwrap_or(u32::MAX);
                        match p.transform(&facts(BASKET, player, worded, seq, t_ms, CELL, C1, asleep, held, moved, now)) {
                            TransformVerdict::Accepted => {
                                prop_assert_eq!(player, owner);
                                prop_assert!(!asleep);
                                prop_assert_eq!(worded, epoch);
                                prop_assert!(newer(seq, last_seq));
                                prop_assert_eq!(p.epoch_of(BASKET), epoch);
                                prop_assert_eq!(stream.map(|s| s.0), Some(player));
                                stream = Some((player, now, held));
                                last_seq = seq;
                            }
                            TransformVerdict::Returned(e) | TransformVerdict::Clamped { epoch: e, .. } => {
                                prop_assert_eq!(player, owner);
                                prop_assert!(asleep);
                                prop_assert_eq!(worded, epoch);
                                prop_assert!(newer(seq, last_seq));
                                prop_assert_eq!(e, raised);
                                stream = None;
                                last_seq = 0;
                            }
                            TransformVerdict::Woken(e) => {
                                // the host alone wakes a prop, and only one nobody streams
                                prop_assert_eq!(player, C1);
                                prop_assert!(!asleep);
                                prop_assert_eq!(worded, epoch);
                                prop_assert_eq!(e, raised);
                                prop_assert!(stream.is_none());
                                stream = Some((player, now, held));
                                last_seq = 0;
                            }
                            TransformVerdict::NotOwner => {
                                prop_assert!(player != owner);
                                prop_assert_eq!(worded, epoch);
                                prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            }
                            TransformVerdict::Stale => {
                                prop_assert!(worded < epoch);
                                prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            }
                            TransformVerdict::Future => {
                                prop_assert!(worded > epoch);
                                prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            }
                            TransformVerdict::Late => {
                                prop_assert_eq!(player, owner);
                                prop_assert_eq!(worded, epoch);
                                prop_assert!(!newer(seq, last_seq));
                                prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            }
                            TransformVerdict::TooFast => {
                                prop_assert_eq!(player, owner);
                                prop_assert_eq!(worded, epoch);
                                prop_assert!(!asleep);
                                prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            }
                            TransformVerdict::Away => {
                                // a live owner's own stream passes its unload's state
                                prop_assert!(stream.map(|s| s.0) != Some(player));
                                prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            }
                            // the cell is always one and always hosted here
                            TransformVerdict::Outside | TransformVerdict::Frozen => return Err(TestCaseError::fail("no cell or no host")),
                        }
                    }
                    4 => {
                        let returned = p.gone(player);
                        if stream.is_some_and(|s| s.0 == player) {
                            prop_assert_eq!(returned, vec![(BASKET, raised)]);
                            stream = None;
                            last_seq = 0;
                        } else {
                            prop_assert_eq!(returned, vec![]);
                        }
                    }
                    _ if asleep => {
                        let returned = p.unloaded(player, CELL);
                        if stream.is_some_and(|s| s.0 == player) {
                            prop_assert_eq!(returned, vec![(BASKET, raised)]);
                            stream = None;
                            last_seq = 0;
                        } else {
                            prop_assert_eq!(returned, vec![]);
                        }
                    }
                    _ => p.loaded(player, CELL),
                }
                // one owner, by the word alone (A1): the table's live row is
                // the stream's, and it names its owner whoever hosts the cell;
                // with none, the host does; and the epoch never falls
                let live = p.live_owner(BASKET);
                prop_assert_eq!(live, stream.map_or(0, |s| s.0));
                for host in [0, C1, C2, C3] {
                    prop_assert_eq!(p.owner_of(BASKET, host), if live == 0 { host } else { live });
                }
                prop_assert!(p.epoch_of(BASKET) >= epoch);
            }
        }

        #[test]
        fn a_player_never_holds_two_props(
            // (which prop, which seat, a touch 0 to 3 or 4 for a sample, the
            // gap before it, the sample's hold)
            ops in prop::collection::vec((0_usize..3, 0_usize..3, 0_u8..5, 0_u64..400, any::<bool>()), 1..200),
        ) {
            // one hand per player (CommonLibSSE-NG P/PlayerCharacter.h:697): whatever three seats claim and stream
            // over three props, their samples' holds included, no seat's rows ever hold two at once (a new grab ends
            // the old hold, and a held sample takes the hand only while it is free)
            let seats = [C1, C2, C3];
            let refrs = [BASKET, 0x54a44, 0x1234];
            let mut p = Props::default();
            let mut now = 1_000_u64;
            for (which, seat, what, gap, held) in ops {
                now = now.saturating_add(gap);
                let refr = refrs.get(which).copied().unwrap_or(BASKET);
                let player = seats.get(seat).copied().unwrap_or(C1);
                if what < 4 {
                    let _ = p.claim(refr, player, CELL, what, now);
                } else {
                    let epoch = p.epoch_of(refr);
                    let seq = p.rows.get(&refr).map_or(1, |r| r.last_seq.checked_add(1).unwrap_or(1));
                    let t_ms = u32::try_from(now).unwrap_or(u32::MAX);
                    let _ = p.transform(&facts(refr, player, epoch, seq, t_ms, CELL, C1, gap % 2 == 0, held, 10.0, now));
                }
                for s in seats {
                    let held = p.rows().iter().filter(|(_, row)| row.owner == s && row.grabbed).count();
                    prop_assert!(held <= 1, "{:x} holds {} props", s, held);
                }
            }
        }

        #[test]
        fn a_pickup_reads_what_a_contact_claim_reads(
            // (which seat, a touch 0 to 3 or 4 for a sample, asleep, the sample's hold, its epoch against the
            // prop's (0 one below, 1 the prop's, 2 one above), the gap before it)
            ops in prop::collection::vec((0_usize..3, 0_u8..5, any::<bool>(), any::<bool>(), 0_u32..3, 0_u64..1_000), 1..200),
        ) {
            // a pickup is a take (ADR-071 decision 2's table): over any interleaving of three seats' claims and
            // samples, a pickup reads Held where a contact claim at the same moment is Held, Moving where it is
            // Moving, and is taken where the contact is granted or the picker's own (no unload here: Away is a late
            // claim's state, which a pickup never asks); on physics rung 2's rows, where an accepted sample's hold
            // sets the grab and a sample of another epoch, or out of order, changes nothing
            let seats = [C1, C2, C3];
            let mut p = Props::default();
            let mut now = 1_000_u64;
            for (seat, what, asleep, held, epoch_at, gap) in ops {
                now = now.saturating_add(gap);
                let player = seats.get(seat).copied().unwrap_or(C1);
                let picked = p.pickup(BASKET, player, now);
                if what == touch::CONTACT {
                    let read = match p.claim(BASKET, player, CELL, touch::CONTACT, now) {
                        ClaimVerdict::Held => PickupVerdict::Held,
                        ClaimVerdict::Moving => PickupVerdict::Moving,
                        ClaimVerdict::Granted(_) | ClaimVerdict::Same => PickupVerdict::Taken,
                        ClaimVerdict::Away => return Err(TestCaseError::fail("no unload here")),
                    };
                    prop_assert_eq!(picked, read);
                } else if what < 4 {
                    let _ = p.claim(BASKET, player, CELL, what, now);
                } else {
                    let epoch = p.epoch_of(BASKET).saturating_add(epoch_at).saturating_sub(1);
                    let seq = p.rows.get(&BASKET).map_or(1, |r| r.last_seq.checked_add(1).unwrap_or(1));
                    let t_ms = u32::try_from(now).unwrap_or(u32::MAX);
                    let _ = p.transform(&facts(BASKET, player, epoch, seq, t_ms, CELL, C1, asleep, held, 10.0, now));
                }
            }
        }
    }
}
