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
//! position.

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
/// The most a row's allowance holds: one honest 100 ms step at
/// `PROP_MAX_SPEED_UNITS_PER_S` (the client's cadence), which a new row
/// starts with (the first sample after a grant, jitter). It replaces a
/// per-sample floor of 128 units, which the review of 9ff1895e (finding 3)
/// found was the limit at the prop budget's 96 samples a second: 12,288
/// units a second, twice the cap. The allowance bounds the record's travel
/// over any window to the cap's plus this one step. UNMEASURED.
pub const PROP_STEP_ALLOWANCE_UNITS: f32 = PROP_MAX_SPEED_UNITS_PER_S / 10.0;

/// What the core knows about a simulator's sample of a prop.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SampleFacts {
    /// The prop's reference id.
    pub refr: u32,
    /// The sender's actor id.
    pub player: u32,
    /// The cell the sample's position lands in, 0 for none.
    pub cell: u32,
    /// The host of the cell the RECORD stands in, 0 while nobody hosts it.
    pub cell_host: u32,
    /// The body's reported state.
    pub asleep: bool,
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
    /// The owner's engine holds the prop (a grab since its last claim, no
    /// release or rest yet): another player's claim is refused as Held. The
    /// holder's client reads its engine's grab at every sample
    /// (Game.getPlayerGrabbedRef, D-PH3 A), claims a grab only while the
    /// engine names the body (havok-props.md E43), sends the release (a drop
    /// claim) when the hold ends and never rests a held body on the wire
    /// (A6), so the holder's asleep sample is a hold already ended too.
    pub grabbed: bool,
    /// The last accepted sample's time (the grant's before any): the moving
    /// window's clock.
    pub last_sample_ms: u64,
    /// The step budget: a token bucket on distance, in units, as it stood at
    /// `allowance_ms`. An accepted sample spends the units it moved the
    /// record; time refills it at `PROP_MAX_SPEED_UNITS_PER_S` up to
    /// `PROP_STEP_ALLOWANCE_UNITS`; a refused sample spends nothing and moves
    /// no clock. It is the prop's, not its owner's: a grant, a host's wake or
    /// a return leaves it as it stands, so seats trading a prop by claims
    /// cannot each start with a full one.
    pub allowance: f32,
    /// When `allowance` was last brought up to date: the last accepted
    /// sample's time, or the row's first (a new row starts full).
    pub allowance_ms: u64,
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

/// The answer to a simulator's transform (ADR-058 decision 2; A4, A7).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransformVerdict {
    /// The sender simulates the prop: the record takes the transform.
    Accepted,
    /// Another seat simulates it (the owner, or the cell's host by default).
    NotOwner,
    /// The sample's point stands in no cell (ADR-071 decision 6's
    /// conservation: no record outside a cell): refused, the record stands,
    /// whoever sent it.
    Outside,
    /// The sample is farther from the record than the step budget allows
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
    /// Refused as farther than the step budget allows, but the sample said
    /// asleep: the rest is taken (the prop is the host's again at this
    /// epoch, the word goes out), the position is not (the record stands);
    /// without it a refused final sample left a live row nobody could move.
    RestRefused(u32),
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

    /// The allowance at `now_ms`: the stored units refilled at
    /// `PROP_MAX_SPEED_UNITS_PER_S` since `allowance_ms`, at most
    /// `PROP_STEP_ALLOWANCE_UNITS`.
    fn allowance_at(&self, now_ms: u64) -> f32 {
        // the time base, clamped to ten seconds (a tenth of one refills the
        // whole allowance)
        let elapsed_ms = u16::try_from(now_ms.saturating_sub(self.allowance_ms).min(10_000)).unwrap_or(10_000);
        (self.allowance + PROP_MAX_SPEED_UNITS_PER_S * f32::from(elapsed_ms) / 1_000.0).min(PROP_STEP_ALLOWANCE_UNITS)
    }
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
    /// from a game that unloaded the cell is late (Away).
    pub fn claim(&mut self, refr: u32, player: u32, cell: u32, touch: u8, now_ms: u64) -> ClaimVerdict {
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
        // the allowance is the prop's: a grant keeps it as it stands, a new
        // row starts with one full step
        let (allowance, allowance_ms) = self
            .rows
            .get(&refr)
            .map_or((PROP_STEP_ALLOWANCE_UNITS, now_ms), |row| (row.allowance, row.allowance_ms));
        self.rows.insert(
            refr,
            PropRow {
                owner: player,
                epoch,
                cell,
                asleep: false,
                grabbed: touch == touch::GRAB,
                last_sample_ms: now_ms,
                allowance,
                allowance_ms,
            },
        );
        ClaimVerdict::Granted(epoch)
    }

    /// `player` reports `refr`'s transform, landing in `cell` (0 for none)
    /// whose record's cell is hosted by `cell_host` (0 while nobody), the
    /// body `asleep` or not, `moved` units from the record, at `now_ms`: a
    /// point in no cell is refused whoever sends it (Outside), a resting prop
    /// in a cell nobody hosts freezes (Frozen); then taken from the prop's
    /// owner alone (`owner_of`) and inside the step budget (the row's
    /// allowance, which the step then spends). Every asleep sample returns
    /// the prop to the host at once, the epoch up (Returned: the core sends
    /// the word with player 0), the host's own rest and a holder's included,
    /// and an asleep sample over the budget takes the rest without the
    /// position (RestRefused); a host's awake sample of a prop that is not
    /// its live row wakes it (Woken: the row the host's at a new epoch, the
    /// word naming the host by id), unless its game reported the cell
    /// unloaded (Away). A live owner's own stream passes wherever it lands.
    pub fn transform(&mut self, f: &SampleFacts) -> TransformVerdict {
        let SampleFacts { refr, player, cell, cell_host, asleep, moved, now_ms } = *f;
        if cell == 0 {
            return TransformVerdict::Outside;
        }
        let owner = self.owner_of(refr, cell_host);
        if owner == 0 {
            return TransformVerdict::Frozen;
        }
        if owner != player {
            return TransformVerdict::NotOwner;
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
        // the step budget: whatever the sample rate, the record travels at
        // most the cap's pace over any window plus one full step (a prop
        // with no row yet holds that step)
        let allowed = self.rows.get(&refr).map_or(PROP_STEP_ALLOWANCE_UNITS, |row| row.allowance_at(now_ms));
        if !moved.is_finite() || moved > allowed {
            if asleep {
                // a refused final sample still ends the owner's turn: the
                // record stands where it was, the prop is the host's again,
                // and the refused step spends nothing
                let next = self.epoch_of(refr).saturating_add(1);
                self.epochs.insert(refr, next);
                let row = self.rows.entry(refr).or_insert(PropRow {
                    owner: 0,
                    epoch: next,
                    cell,
                    asleep: true,
                    grabbed: false,
                    last_sample_ms: now_ms,
                    allowance: PROP_STEP_ALLOWANCE_UNITS,
                    allowance_ms: now_ms,
                });
                row.owner = 0;
                row.grabbed = false;
                row.asleep = true;
                row.epoch = next;
                return TransformVerdict::RestRefused(next);
            }
            return TransformVerdict::TooFast;
        }
        let epoch = self.epoch_of(refr);
        let row = self.rows.entry(refr).or_insert(PropRow {
            owner,
            epoch,
            cell,
            asleep: false,
            grabbed: false,
            last_sample_ms: now_ms,
            allowance: PROP_STEP_ALLOWANCE_UNITS,
            allowance_ms: now_ms,
        });
        if row.owner != owner {
            // the host's sample of a prop that was not its live row (a
            // resting one): the host's row now, with no grab mark
            row.owner = owner;
            row.grabbed = false;
        }
        row.cell = cell;
        row.asleep = asleep;
        row.last_sample_ms = now_ms;
        // the step spends what it moved the record (a distance, never below 0)
        row.allowance = (allowed - moved.max(0.0)).max(0.0);
        row.allowance_ms = now_ms;
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
            row.epoch = next;
            self.epochs.insert(refr, next);
            return TransformVerdict::Returned(next);
        }
        if !own_live {
            // the host's engine moved a prop nobody streamed: the word names
            // the host by id, so the seat that streams is the seat every
            // word names (a host change leaves it streaming until its rest)
            let next = epoch.saturating_add(1);
            row.epoch = next;
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
            row.epoch = next;
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
            row.epoch = next;
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

    fn facts(refr: u32, player: u32, cell: u32, cell_host: u32, asleep: bool, moved: f32, now_ms: u64) -> SampleFacts {
        SampleFacts { refr, player, cell, cell_host, asleep, moved, now_ms }
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
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_500)), TransformVerdict::Accepted);
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
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, false, 1.0, 1_500)), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 601_500), ClaimVerdict::Held);
        assert_eq!(p.live_owner(BASKET), C1);
        // the holder's release clears the grab (its client's drop claim when
        // the engine lets go, by a key or by itself: havok-props.md E40 and
        // E41); the fall streams, a contact on it is moving, and the rest
        // returns it to the host with the word
        assert_eq!(p.claim(BASKET, C1, CELL, touch::DROP, 601_600), ClaimVerdict::Same);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, false, 10.0, 601_700)), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 601_750), ClaimVerdict::Moving);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, true, 10.0, 601_800)), TransformVerdict::Returned(3));
        assert_eq!(p.owner_of(BASKET, C2), C2);
        assert_eq!(p.epoch_of(BASKET), 3);
        // a hold whose release claim never came ends at the holder's rest: a
        // held body never rests on the wire (A6), so the asleep sample is a
        // hold already ended, and the word goes out
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_500)), TransformVerdict::Accepted);
        assert_eq!(q.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 2_000)), TransformVerdict::Returned(2));
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
    fn only_the_owner_moves_the_record_and_its_rest_returns_the_prop_to_the_host_as_a_word() {
        let mut p = Props::default();
        // the host's own sample of an unclaimed prop wakes it: the row the host's at epoch 1, the word naming the host
        // by id; its next sample is the live row's; its rest returns it with a word too (player 0, epoch 2)
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 500)), TransformVerdict::Woken(1));
        assert_eq!(p.live_owner(BASKET), C1);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 550)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 600)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, true, 10.0, 700)), TransformVerdict::Returned(2));
        assert_eq!(p.epoch_of(BASKET), 2);
        assert_eq!(p.owner_of(BASKET, C1), C1);
        // c2's touch at rest: granted; only c2's samples pass
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(3));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_100)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_100)), TransformVerdict::Accepted);
        // no timer hands it back: a long quiet keeps c2 the owner, the host's own sample refused as any non-owner's
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 60_000)), TransformVerdict::NotOwner);
        assert_eq!(p.owner_of(BASKET, C1), C2);
        // c2's own asleep sample: the host's again at once, at the next epoch, and the word goes out
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 61_000)), TransformVerdict::Returned(4));
        assert_eq!(p.owner_of(BASKET, C1), C1);
        assert_eq!(p.live_owner(BASKET), 0);
        assert_eq!(p.epoch_of(BASKET), 4);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 61_100)), TransformVerdict::NotOwner);
        // the host's kick of the resting prop is a wake, by id (epoch 5); its rest returns it again (6); c2's new touch
        // at rest is a grant at 7
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 61_100)), TransformVerdict::Woken(5));
        assert_eq!(p.live_owner(BASKET), C1);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, true, 10.0, 61_200)), TransformVerdict::Returned(6));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 62_000), ClaimVerdict::Granted(7));
    }

    #[test]
    fn a_prop_in_no_cell_or_an_unhosted_unclaimed_cell_freezes() {
        let mut p = Props::default();
        // no cell at the sample's point: refused whoever sends it (ADR-071 decision 6's conservation)
        assert_eq!(p.transform(&facts(BASKET, C1, 0, C1, false, 10.0, 0)), TransformVerdict::Outside);
        // a resting prop in a cell nobody hosts: frozen at the record
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, false, 10.0, 0)), TransformVerdict::Frozen);
        // a live toucher keeps simulating in a cell whose host left, never past the world's cells; its rest returns
        // the prop to nobody, frozen again
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, false, 10.0, 1_100)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C1, 0, 0, false, 10.0, 1_150)), TransformVerdict::Outside);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, true, 10.0, 1_200)), TransformVerdict::Returned(2));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, false, 10.0, 1_300)), TransformVerdict::Frozen);
    }

    #[test]
    fn a_host_change_changes_no_row_the_live_ones_stay_and_the_resting_ones_are_the_new_hosts() {
        // A7: a grant changes no row (the table has no entry for it): a resting row names nobody already, so the new
        // host simulates it by default; a live row stays its owner's until its rest
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 1_500)), TransformVerdict::Returned(2));
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 1_600), ClaimVerdict::Granted(1));
        let rows = p.rows();
        assert!(rows.iter().any(|(refr, row)| *refr == BASKET && row.owner == 0 && row.epoch == 2));
        // c3 hosts the cell now: the basket is its, the cabbage still c2's, nothing written
        assert_eq!(p.owner_of(BASKET, C3), C3);
        assert_eq!(p.owner_of(0x54a44, C3), C2);
        assert_eq!(p.rows(), rows);
        assert_eq!(p.epoch_of(BASKET), 2);
        // the new host's sample of the cabbage is refused until c2's rest returns it, then its own wake takes it by id
        assert_eq!(p.transform(&facts(0x54a44, C3, CELL, C3, false, 10.0, 1_700)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(0x54a44, C2, CELL, C3, true, 10.0, 1_800)), TransformVerdict::Returned(2));
        assert_eq!(p.transform(&facts(0x54a44, C3, CELL, C3, false, 10.0, 1_900)), TransformVerdict::Woken(3));
    }

    #[test]
    fn a_players_unload_of_a_cell_returns_its_live_rows_there_with_a_word_each() {
        // the review of 2026-11-03 (D2; ADR-071's D-PH2 A): the old host's cell unloads mid-flight (a door, a
        // teleport); its 3D of the prop is gone and the rescan untracks it before any final sample, so the row would
        // stay live naming it and nobody would stream; the unload returns its rows there to the host by default with a
        // word each, as a departure does
        let mut p = Props::default();
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_000)), TransformVerdict::Woken(1));
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::GRAB, 1_100), ClaimVerdict::Granted(1));
        // a resting row and another cell's row are not the unload's
        assert_eq!(p.claim(0x1234, C1, CELL, touch::CONTACT, 1_200), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(0x1234, C1, CELL, C1, true, 10.0, 1_300)), TransformVerdict::Returned(2));
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
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 2_050)), TransformVerdict::Away);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 2_060), ClaimVerdict::Away);
        assert_eq!(p.transform(&facts(0x5678, C1, 0x9733, C1, false, 10.0, 2_070)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(0x5678, C1, CELL, C1, false, 10.0, 2_075)), TransformVerdict::Accepted);
        p.reached(C1, &[0x9733, CELL]);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 2_080)), TransformVerdict::Woken(3));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, true, 10.0, 2_090)), TransformVerdict::Returned(4));
        // the second unload returns the cabbage alone: its stream landed it in this cell at 2_075, the basket rests
        assert_eq!(p.unloaded(C1, CELL), vec![(0x5678, 2)]);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 2_096)), TransformVerdict::Away);
        p.loaded(C1, CELL);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 2_097)), TransformVerdict::Woken(5));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, true, 10.0, 2_098)), TransformVerdict::Returned(6));
        // the holder's unload drops its grab too (a grab carried through a door stays in no hand), and its claim
        // there waits for its load report as well; a departure clears the state
        assert_eq!(p.unloaded(C2, CELL), vec![(0x54a44, 2)]);
        assert_eq!(p.live_owner(0x54a44), 0);
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::GRAB, 2_150), ClaimVerdict::Away);
        assert_eq!(p.gone(C2), vec![]);
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 2_300), ClaimVerdict::Granted(3));
        // the live owner's own stream into a cell it reported unloaded passes (its row is live), a wake there does not
        assert_eq!(p.unloaded(C2, 0x9733), vec![]);
        assert_eq!(p.transform(&facts(0x54a44, C2, 0x9733, C1, false, 10.0, 2_410)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(0x54a44, C2, 0x9733, C1, true, 10.0, 2_420)), TransformVerdict::Returned(4));
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
        assert_eq!(p.transform(&facts(0x1234, C2, CELL, C1, true, 10.0, 1_300)), TransformVerdict::Returned(2));
        assert_eq!(p.gone(C2), vec![(BASKET, 2), (0x54a44, 2)]);
        assert_eq!(p.owner_of(BASKET, C1), C1);
        assert_eq!(p.owner_of(BASKET, 0), 0);
        assert_eq!(p.epoch_of(BASKET), 2);
        assert_eq!(p.epoch_of(0x1234), 2);
        // the host's samples pass and it streams the prop awake (its wake, by id, epoch 3): c2's return touch waits
        // for the host's rest (a word, epoch 4), then is a grant at 5
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 2_200)), TransformVerdict::Woken(3));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 2_300), ClaimVerdict::Moving);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, true, 10.0, 2_400)), TransformVerdict::Returned(4));
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
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_000)), TransformVerdict::Woken(1));
        assert_eq!(p.live_owner(BASKET), C1);
        // c2 hosts the cell from here
        assert_eq!(p.owner_of(BASKET, C2), C1);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C2, false, 10.0, 1_200)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, false, 10.0, 1_200)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, true, 10.0, 1_300)), TransformVerdict::Returned(2));
        assert_eq!(p.owner_of(BASKET, C2), C2);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C2, false, 10.0, 1_400)), TransformVerdict::Woken(3));
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C2, true, 10.0, 1_500)), TransformVerdict::Returned(4));
        // a non-host's sample never wakes a resting prop
        assert_eq!(p.transform(&facts(BASKET, C3, CELL, C2, false, 10.0, 1_600)), TransformVerdict::NotOwner);
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
        assert_eq!(p.transform(&facts(ROCK, C2, MINE, C1, false, 40.0, 1_100)), TransformVerdict::Accepted);
        assert_eq!(p.claim(ROCK, C1, MINE, touch::IMPULSE, 1_150), ClaimVerdict::Moving);
        assert_eq!(p.claim(ROCK, C3, MINE, touch::IMPULSE, 1_150), ClaimVerdict::Moving);
        assert_eq!(p.owner_of(ROCK, C1), C2);
        assert_eq!(p.epoch_of(ROCK), 1);
        // the firing seat's own claim again is Same
        assert_eq!(p.claim(ROCK, C2, MINE, touch::IMPULSE, 1_200), ClaimVerdict::Same);
        // the rest sample returns the rock to the host with the word (epoch 2), and a new firing takes the resting
        // body at the next epoch
        assert_eq!(p.transform(&facts(ROCK, C2, MINE, C1, true, 40.0, 1_300)), TransformVerdict::Returned(2));
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
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_100)), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::IMPULSE, 1_150), ClaimVerdict::Moving);
        assert_eq!(p.owner_of(BASKET, C1), C2);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::GRAB, 1_200), ClaimVerdict::Granted(2));
        assert_eq!(p.owner_of(BASKET, C2), C1);
    }

    #[test]
    fn a_sample_farther_than_the_step_budget_is_refused_and_leaves_the_record() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        // the first sample after the grant: a new row's full allowance, one
        // 100 ms step at the cap
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS + 1.0, 1_010)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 500.0, 1_010)), TransformVerdict::Accepted);
        // 100 units left, and 50 ms later 300 more at 6000 a second
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 401.0, 1_060)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 400.0, 1_060)), TransformVerdict::Accepted);
        // a refused sample spends nothing and moves no clock: the next is
        // judged from the last accepted, 40 ms of refill
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 5_000.0, 1_100)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 241.0, 1_100)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 240.0, 1_100)), TransformVerdict::Accepted);
        // a long pause refills one full step and never more: no banked
        // teleport after a silence
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS + 1.0, 2_900)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 2_900)), TransformVerdict::Accepted);
        // not a number: refused
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, f32::NAN, 3_000)), TransformVerdict::TooFast);
        // an asleep sample over the budget: the rest is taken (the prop the
        // host's again, a word), the position not
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 5_000.0, 3_100)), TransformVerdict::RestRefused(2));
        assert_eq!(p.owner_of(BASKET, C1), C1);
        assert_eq!(p.live_owner(BASKET), 0);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 3_200)), TransformVerdict::NotOwner);
        // the host's first sample of an unclaimed prop: a new row's full
        // allowance, which the step spends; the first is its wake
        let mut q = Props::default();
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS + 1.0, 0)), TransformVerdict::TooFast);
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, 200.0, 0)), TransformVerdict::Woken(1));
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, 401.0, 0)), TransformVerdict::TooFast);
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, 400.0, 0)), TransformVerdict::Accepted);
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
            let verdict = p.transform(&facts(BASKET, C2, CELL, C1, false, 128.0, 1_000 + i * 1_000 / 96));
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
            assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 1_000 + i * 100)), TransformVerdict::Accepted);
        }
        // a kick at the engine's 7 m/s (476 units a second, thuum docs/verbs/
        // prop-physics.md's x-prop-launch v3 read): 47.6 a step at 10 Hz, and
        // 5 a step at the prop budget's 96 a second
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        for i in 1..=100_u64 {
            assert_eq!(q.transform(&facts(BASKET, C2, CELL, C1, false, 47.6, 1_000 + i * 100)), TransformVerdict::Accepted);
        }
        for i in 1..=960_u64 {
            assert_eq!(q.transform(&facts(BASKET, C2, CELL, C1, false, 5.0, 11_000 + i * 1_000 / 96)), TransformVerdict::Accepted);
        }
        // the prop budget's whole 96 samples a second spent on one trap body
        // falling at 4,800 units a second (50 a step): taken whole, since
        // the allowance judges distance, never the rate
        let mut r = Props::default();
        assert_eq!(r.claim(ROCK, C2, MINE, touch::IMPULSE, 1_000), ClaimVerdict::Granted(1));
        for i in 1..=960_u64 {
            assert_eq!(r.transform(&facts(ROCK, C2, MINE, C1, false, 50.0, 1_000 + i * 1_000 / 96)), TransformVerdict::Accepted);
        }
    }

    #[test]
    fn a_grant_leaves_the_rows_allowance_as_it_stands() {
        // the allowance is the prop's, refilled by time alone: seats trading
        // a prop by claims cannot each start with a full one (c3's grab takes
        // the moving prop, A2, the allowance as c2 left it)
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 1_000)), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C3, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(2));
        assert_eq!(p.transform(&facts(BASKET, C3, CELL, C1, false, 1.0, 1_000)), TransformVerdict::TooFast);
        // 50 ms later, 300 units refilled at 6000 a second
        assert_eq!(p.transform(&facts(BASKET, C3, CELL, C1, false, 301.0, 1_050)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C3, CELL, C1, false, 300.0, 1_050)), TransformVerdict::Accepted);
    }

    proptest! {
        #[test]
        fn no_owners_stream_carries_a_prop_past_the_cap(
            steps in prop_oneof![
                // a crafted stream: gaps far under the client's 100 ms, each
                // step past the cap's pace for its gap
                prop::collection::vec((0_u64..10, 64.0_f32..160.0), 1..300),
                // anything: long gaps, steps past the allowance
                prop::collection::vec((0_u64..250, 0.0_f32..900.0), 1..300),
                // the two mixed, sample by sample
                prop::collection::vec(prop_oneof![(0_u64..10, 64.0_f32..160.0), (0_u64..250, 0.0_f32..900.0)], 1..300),
            ],
        ) {
            // whatever one owner streams, at any pace and with any step, the
            // record moves no farther than the cap allows over the stream's
            // length, plus the one step the allowance holds at most
            let mut p = Props::default();
            prop_assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
            let mut now = 1_000_u64;
            let mut carried = 0.0_f64;
            for (gap, moved) in steps {
                now = now.saturating_add(gap);
                if p.transform(&facts(BASKET, C2, CELL, C1, false, moved, now)) == TransformVerdict::Accepted {
                    carried += f64::from(moved);
                    let elapsed_s = f64::from(u32::try_from(now.saturating_sub(1_000)).unwrap_or(u32::MAX)) / 1_000.0;
                    let bound = f64::from(PROP_MAX_SPEED_UNITS_PER_S) * elapsed_s + f64::from(PROP_STEP_ALLOWANCE_UNITS);
                    prop_assert!(carried <= bound + 0.5, "carried {} past {} at {} ms", carried, bound, now);
                }
            }
        }

        #[test]
        fn three_seats_keep_one_owner_and_every_word_raises_the_epoch(
            // (what, which seat, the touch, asleep (for an unload: unload or load), the step, the gap before it)
            ops in prop::collection::vec((0_u8..6, 0_usize..3, 0_u8..4, any::<bool>(), 0.0_f32..50.0, 0_u64..700), 1..200),
        ) {
            // A1 to A7 over any interleaving of three seats' claims, samples,
            // departures and unloads, c1 hosting the cell: the record moves for
            // the owner alone; every word raises the epoch by one and nothing
            // else moves it; a live row names its owner whoever hosts; and no
            // contact, drop or impulse takes a row another seat holds or
            // streams inside its moving window
            let seats = [C1, C2, C3];
            let mut p = Props::default();
            let mut now = 1_000_u64;
            // the stream as the words left it: its owner, its last accepted
            // sample (or the grant's or the claim's time) and its grab mark
            let mut stream: Option<(u32, u64, bool)> = None;
            for (what, seat, touch, asleep, moved, gap) in ops {
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
                    2 | 3 => match p.transform(&facts(BASKET, player, CELL, C1, asleep, moved, now)) {
                        TransformVerdict::Accepted => {
                            prop_assert_eq!(player, owner);
                            prop_assert!(!asleep);
                            prop_assert_eq!(p.epoch_of(BASKET), epoch);
                            let held = stream.is_some_and(|s| s.2);
                            prop_assert_eq!(stream.map(|s| s.0), Some(player));
                            stream = Some((player, now, held));
                        }
                        TransformVerdict::Returned(e) | TransformVerdict::RestRefused(e) => {
                            prop_assert_eq!(player, owner);
                            prop_assert!(asleep);
                            prop_assert_eq!(e, raised);
                            stream = None;
                        }
                        TransformVerdict::Woken(e) => {
                            // the host alone wakes a prop, and only one nobody streams
                            prop_assert_eq!(player, C1);
                            prop_assert!(!asleep);
                            prop_assert_eq!(e, raised);
                            prop_assert!(stream.is_none());
                            stream = Some((player, now, false));
                        }
                        TransformVerdict::NotOwner => {
                            prop_assert!(player != owner);
                            prop_assert_eq!(p.epoch_of(BASKET), epoch);
                        }
                        TransformVerdict::TooFast | TransformVerdict::Away => {
                            prop_assert_eq!(player, owner);
                            prop_assert_eq!(p.epoch_of(BASKET), epoch);
                        }
                        // the cell is always one and always hosted here
                        TransformVerdict::Outside | TransformVerdict::Frozen => return Err(TestCaseError::fail("no cell or no host")),
                    },
                    4 => {
                        let returned = p.gone(player);
                        if stream.is_some_and(|s| s.0 == player) {
                            prop_assert_eq!(returned, vec![(BASKET, raised)]);
                            stream = None;
                        } else {
                            prop_assert_eq!(returned, vec![]);
                        }
                    }
                    _ if asleep => {
                        let returned = p.unloaded(player, CELL);
                        if stream.is_some_and(|s| s.0 == player) {
                            prop_assert_eq!(returned, vec![(BASKET, raised)]);
                            stream = None;
                        } else {
                            prop_assert_eq!(returned, vec![]);
                        }
                    }
                    _ => p.loaded(player, CELL),
                }
                // one owner, by the word alone (A1): the table's live row is
                // the stream's, and it names its owner whoever hosts the cell;
                // with none, the host does
                let live = p.live_owner(BASKET);
                prop_assert_eq!(live, stream.map_or(0, |s| s.0));
                for host in [0, C1, C2, C3] {
                    prop_assert_eq!(p.owner_of(BASKET, host), if live == 0 { host } else { live });
                }
            }
        }
    }
}
