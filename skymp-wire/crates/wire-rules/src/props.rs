//! Physics props (thuum docs/verbs/prop-physics.md, ADR-058): one simulating
//! seat per prop, Garry's Mod's shape without its server. A prop (a kicked
//! basket, a dropped sword) is simulated by exactly one seat at a time: the
//! cell's host by default, and the seat whose player last touched it for as
//! long as its lease holds. The server decides who simulates (the claim, R0:
//! newest touch wins, one epoch per prop) and records the simulator's
//! transforms (R2: only the owner's are taken; an asleep sample starts the
//! lease, after which the cell's host simulates again). The owner table is
//! ADR-050 decision 4's second row kind, "prop", beside the cells rule's
//! follows, margins and claims (`cells.rs`); the cell is still the policy: a
//! prop in a cell nobody hosts and nobody holds freezes at the record. The
//! core gathers the facts (who hosts the prop's cell, where the record
//! stands, the wall clock) and asks here; nothing here knows a position.

use std::collections::HashMap;

/// How long a toucher keeps a prop after its body sleeps, before the cell's
/// host simulates it again (ADR-058 decision 1). UNMEASURED: x-prop-push
/// sizes it.
pub const PROP_LEASE_MS: u64 = 3_000;

/// How long an awake row stays its owner's with no accepted sample: a seat
/// that stopped streaming (the prop left its range, its body was lost, the
/// game hung) hands the prop back to the cell's host instead of holding it
/// frozen mid-air for the session. The client sends at every 2 units of
/// motion and a final asleep sample, so silence means a fault. UNMEASURED.
pub const PROP_SILENCE_MS: u64 = 3_000;

/// The longest a grab holds a prop against every other claim without the
/// holder's drop: a safety against a release the holder's seat never
/// reported. A grabbed row neither sleeps into a lease nor lapses in silence
/// (a held object stands still in the hand).
pub const PROP_GRAB_MAX_MS: u64 = 120_000;

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
    /// A shout's or spell's impulse.
    pub const IMPULSE: u8 = 3;
}

/// One prop's row: who simulates it and on what terms.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PropRow {
    /// The simulating player's actor form id; 0 once it left (the cell's
    /// host simulates, `owner_of`).
    pub owner: u32,
    /// The prop's owner epoch at the last grant; kept across fallbacks and
    /// host changes, up only at a grant (the wire's `PropOwner.epoch`).
    pub epoch: u32,
    /// The cell the prop's record stands in, by the last claim or accepted
    /// transform.
    pub cell: u32,
    /// While the body sleeps: when the toucher's lease ends; none while it is
    /// awake (the lease has not started).
    pub lease_until_ms: Option<u64>,
    /// The body's last reported state.
    pub asleep: bool,
    /// The owner holds the prop in its hands (a grab since its last claim,
    /// no drop yet): another player's claim is refused as Held. HYPOTHESIS:
    /// the engine's grab is exclusive on the grabbing seat, so a contact
    /// from another seat cannot take it without breaking the grab there.
    pub grabbed: bool,
    /// When this owner took the row (the grant, or the host's takeover of a
    /// lapsed row): a grab's clock.
    pub since_ms: u64,
    /// The last accepted sample's time (the grant's before any): the
    /// silence clock.
    pub last_sample_ms: u64,
    /// The step budget: a token bucket on distance, in units, as it stood at
    /// `allowance_ms`. An accepted sample spends the units it moved the
    /// record; time refills it at `PROP_MAX_SPEED_UNITS_PER_S` up to
    /// `PROP_STEP_ALLOWANCE_UNITS`; a refused sample spends nothing and moves
    /// no clock. It is the prop's, not its owner's: a grant, a host's
    /// takeover or a departure leaves it as it stands, so seats trading a
    /// prop by claims cannot each start with a full one.
    pub allowance: f32,
    /// When `allowance` was last brought up to date: the last accepted
    /// sample's time, or the row's first (a new row starts full).
    pub allowance_ms: u64,
}

/// The answer to a claim (ADR-058 decision 1).
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
    /// A trap's world body another seat's firing set moving, still awake
    /// under that seat's live row: the impulse claim of a second firing is
    /// refused, the first firing wins (thuum D-S3 point 4,
    /// `Props::claim_world_body`); nothing changes.
    Moving,
}

/// The answer to a simulator's transform (ADR-058 decision 2).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransformVerdict {
    /// The sender simulates the prop: the record takes the transform.
    Accepted,
    /// Another seat simulates it (the owner, or the cell's host by default).
    NotOwner,
    /// The prop stands in no cell at all, or in a cell nobody hosts while
    /// nobody holds a live lease on it: it freezes at the record.
    WrongCell,
    /// The sample is farther from the record than the step budget allows
    /// (the row's allowance, `PropRow::allowance`), or not a number:
    /// refused, the record stands.
    TooFast,
}

/// Every prop's row and every prop's epoch (kept after its row is gone).
#[derive(Debug, Default)]
pub struct Props {
    rows: HashMap<u32, PropRow>,
    epochs: HashMap<u32, u32>,
}

impl PropRow {
    /// Whether the row names its owner now: grabbed inside the grab's limit;
    /// awake with a sample inside the silence window; asleep inside the
    /// lease. An owner of 0 never does.
    fn live(&self, now_ms: u64) -> bool {
        if self.owner == 0 {
            return false;
        }
        if self.grabbed {
            return now_ms < self.since_ms.saturating_add(PROP_GRAB_MAX_MS);
        }
        if self.asleep {
            return self.lease_until_ms.is_some_and(|until| now_ms < until);
        }
        now_ms < self.last_sample_ms.saturating_add(PROP_SILENCE_MS)
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
    /// Who simulates `refr` at `now_ms`, given that `cell_host` hosts its
    /// cell (0 while nobody does): the row's owner while the body is awake
    /// or its lease holds, else the host.
    #[must_use]
    pub fn owner_of(&self, refr: u32, cell_host: u32, now_ms: u64) -> u32 {
        match self.rows.get(&refr) {
            Some(row) if row.live(now_ms) => row.owner,
            _ => cell_host,
        }
    }

    /// The prop's owner epoch: the last grant's, 0 before any.
    #[must_use]
    pub fn epoch_of(&self, refr: u32) -> u32 {
        self.epochs.get(&refr).copied().unwrap_or(0)
    }

    /// The row's owner while the row is live, else 0 (the core tells a late
    /// listener who simulates; with 0 the listener falls back to the host).
    #[must_use]
    pub fn live_owner(&self, refr: u32, now_ms: u64) -> u32 {
        match self.rows.get(&refr) {
            Some(row) if row.live(now_ms) => row.owner,
            _ => 0,
        }
    }

    /// `player` touched `refr`, which stands in `cell`, by `touch` at
    /// `now_ms`: the newest touch wins (Granted, the epoch up), unless the
    /// claimant simulates it already (Same) or another player holds it
    /// grabbed (Held). The core has checked that the claimant stands in the
    /// prop's cell. A grant wakes the row: no lease until the body sleeps.
    pub fn claim(&mut self, refr: u32, player: u32, cell: u32, touch: u8, now_ms: u64) -> ClaimVerdict {
        if let Some(row) = self.rows.get_mut(&refr) {
            if row.live(now_ms) {
                if row.owner == player {
                    row.grabbed = touch == touch::GRAB;
                    row.cell = cell;
                    // a touch wakes the row: the holder's drop is followed by
                    // its fall samples, which pass while the silence window runs
                    row.asleep = false;
                    row.lease_until_ms = None;
                    row.last_sample_ms = now_ms;
                    return ClaimVerdict::Same;
                }
                if row.grabbed {
                    return ClaimVerdict::Held;
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
                lease_until_ms: None,
                asleep: false,
                grabbed: touch == touch::GRAB,
                since_ms: now_ms,
                last_sample_ms: now_ms,
                allowance,
                allowance_ms,
            },
        );
        ClaimVerdict::Granted(epoch)
    }

    /// `player`'s seat claims `refr`, a trap's world body (thuum D-S3 point 4:
    /// a world object a seat's own script set moving, Embershard's falling
    /// rocks; never an item, which `claim` judges), standing in `cell`, at
    /// `now_ms`. A world body joins the stream by the impulse alone (the core
    /// admits no other touch on it), so this is `claim` with
    /// `touch::IMPULSE`, but the first firing wins: while another player's
    /// live row has the body awake (moving under that seat's firing, from its
    /// grant to its rest sample), the claim is Moving and nothing changes. A
    /// body at rest (asleep, its lease running) or a lapsed row is any new
    /// firing's to take, and the claimant's own row is Same.
    pub fn claim_world_body(&mut self, refr: u32, player: u32, cell: u32, now_ms: u64) -> ClaimVerdict {
        if let Some(row) = self.rows.get(&refr) {
            if row.live(now_ms) && row.owner != player && !row.grabbed && !row.asleep {
                return ClaimVerdict::Moving;
            }
        }
        self.claim(refr, player, cell, touch::IMPULSE, now_ms)
    }

    /// `player` reports `refr`'s transform, landing in `cell` (0 for none)
    /// whose record's cell is hosted by `cell_host` (0 while nobody), the
    /// body `asleep` or not, `moved` units from the record, at `now_ms`:
    /// taken from the prop's owner alone (`owner_of`) and inside the step
    /// budget (the row's allowance, which the step then spends); a sleep
    /// starts the lease (a grabbed row's does not), a wake ends it; a host's
    /// first sample of an unclaimed prop writes its row at the kept epoch,
    /// and a host's sample of a lapsed row takes it over, ending a grab its
    /// holder never released.
    pub fn transform(&mut self, f: &SampleFacts) -> TransformVerdict {
        let SampleFacts { refr, player, cell, cell_host, asleep, moved, now_ms } = *f;
        if cell == 0 {
            return TransformVerdict::WrongCell;
        }
        let owner = self.owner_of(refr, cell_host, now_ms);
        if owner == 0 {
            return TransformVerdict::WrongCell;
        }
        if owner != player {
            return TransformVerdict::NotOwner;
        }
        // the step budget: whatever the sample rate, the record travels at
        // most the cap's pace over any window plus one full step (a prop
        // with no row yet holds that step)
        let allowed = self.rows.get(&refr).map_or(PROP_STEP_ALLOWANCE_UNITS, |row| row.allowance_at(now_ms));
        if !moved.is_finite() || moved > allowed {
            return TransformVerdict::TooFast;
        }
        let epoch = self.epoch_of(refr);
        let row = self.rows.entry(refr).or_insert(PropRow {
            owner,
            epoch,
            cell,
            lease_until_ms: None,
            asleep: false,
            grabbed: false,
            since_ms: now_ms,
            last_sample_ms: now_ms,
            allowance: PROP_STEP_ALLOWANCE_UNITS,
            allowance_ms: now_ms,
        });
        if row.owner != owner {
            row.owner = owner;
            row.grabbed = false;
            row.since_ms = now_ms;
        }
        row.cell = cell;
        row.asleep = asleep;
        row.last_sample_ms = now_ms;
        row.lease_until_ms = (asleep && !row.grabbed).then(|| now_ms.saturating_add(PROP_LEASE_MS));
        // the step spends what it moved the record (a distance, never below 0)
        row.allowance = (allowed - moved.max(0.0)).max(0.0);
        row.allowance_ms = now_ms;
        TransformVerdict::Accepted
    }

    /// `host` now hosts `cell` (a grant): the rows in the cell nobody holds
    /// live (a lapsed lease, an owner gone) name the host, their epochs kept;
    /// a live toucher's row survives the grant, as the cells rule's claims do.
    pub fn host_changed(&mut self, cell: u32, host: u32, now_ms: u64) {
        for row in self.rows.values_mut().filter(|r| r.cell == cell) {
            if !row.live(now_ms) {
                row.owner = host;
                row.lease_until_ms = None;
                row.grabbed = false;
                row.since_ms = now_ms;
                row.last_sample_ms = now_ms;
            }
        }
    }

    /// `player` left the server: its rows fall back to their cells' hosts
    /// (`owner_of`), their epochs kept.
    pub fn gone(&mut self, player: u32) {
        for row in self.rows.values_mut().filter(|r| r.owner == player) {
            row.owner = 0;
            row.lease_until_ms = None;
            row.grabbed = false;
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
        assert_eq!(p.owner_of(BASKET, C1, 0), C1);
        assert_eq!(p.owner_of(BASKET, 0, 0), 0);
        assert_eq!(p.epoch_of(BASKET), 0);
    }

    #[test]
    fn the_newest_touch_wins_with_an_epoch_per_prop_and_the_same_toucher_keeps_it() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.owner_of(BASKET, C1, 1_000), C2);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_100), ClaimVerdict::Same);
        assert_eq!(p.epoch_of(BASKET), 1);
        // c1 kicks it back: the owner flips at the next epoch
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_200), ClaimVerdict::Granted(2));
        assert_eq!(p.owner_of(BASKET, C1, 1_200), C1);
        // another prop counts its own epochs
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 1_300), ClaimVerdict::Granted(1));
        assert_eq!(p.epoch_of(BASKET), 2);
    }

    #[test]
    fn a_grabbed_prop_is_held_until_its_drop() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C1, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_100), ClaimVerdict::Held);
        assert_eq!(p.owner_of(BASKET, C2, 1_100), C1);
        // the holder's drop clears the grab; the next touch takes it
        assert_eq!(p.claim(BASKET, C1, CELL, touch::DROP, 1_200), ClaimVerdict::Same);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_300), ClaimVerdict::Granted(2));
    }

    #[test]
    fn only_the_owner_moves_the_record_and_a_sleep_starts_the_lease() {
        let mut p = Props::default();
        // the host's sample of an unclaimed prop
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 500)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 600)), TransformVerdict::NotOwner);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_100)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_100)), TransformVerdict::Accepted);
        // the body sleeps: the toucher keeps it for the lease, then the host
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 2_000)), TransformVerdict::Accepted);
        assert_eq!(p.owner_of(BASKET, C1, 2_000 + PROP_LEASE_MS - 1), C2);
        assert_eq!(p.owner_of(BASKET, C1, 2_000 + PROP_LEASE_MS), C1);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 2_000 + PROP_LEASE_MS)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 2_000 + PROP_LEASE_MS)), TransformVerdict::Accepted);
        // a wake inside the lease ends it: the toucher keeps the prop while it
        // streams (the silence window), the lease's end no longer counts
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 2_000)), TransformVerdict::Accepted);
        assert_eq!(q.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 2_500)), TransformVerdict::Accepted);
        assert_eq!(q.owner_of(BASKET, C1, 2_000 + PROP_LEASE_MS), C2);
        assert_eq!(q.owner_of(BASKET, C1, 2_500 + PROP_SILENCE_MS - 1), C2);
        // a lapsed toucher's new touch is a grant again (the host's seat must
        // hear the word), at the next epoch
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 6_000), ClaimVerdict::Granted(2));
    }

    #[test]
    fn a_prop_in_no_cell_or_an_unhosted_unclaimed_cell_freezes() {
        let mut p = Props::default();
        assert_eq!(p.transform(&facts(BASKET, C1, 0, C1, false, 10.0, 0)), TransformVerdict::WrongCell);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, false, 10.0, 0)), TransformVerdict::WrongCell);
        // a live toucher keeps simulating in a cell whose host left
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, false, 10.0, 1_100)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, true, 10.0, 1_200)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, false, 10.0, 1_200 + PROP_LEASE_MS)), TransformVerdict::WrongCell);
    }

    #[test]
    fn a_host_change_takes_the_lapsed_rows_and_leaves_the_live_ones_epochs_kept() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 1_500)), TransformVerdict::Accepted);
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 1_600), ClaimVerdict::Granted(1));
        // c1 takes the cell after the basket's lease lapsed, while the cabbage is live
        p.host_changed(CELL, C1, 1_500 + PROP_LEASE_MS);
        assert_eq!(p.owner_of(BASKET, C1, 1_500 + PROP_LEASE_MS), C1);
        assert_eq!(p.owner_of(0x54a44, C1, 1_500 + PROP_LEASE_MS), C2);
        assert_eq!(p.epoch_of(BASKET), 1);
        let rows = p.rows();
        assert_eq!(rows.len(), 2);
        assert!(rows.iter().any(|(refr, row)| *refr == BASKET && row.owner == C1 && row.epoch == 1));
        // a row in another cell is not the grant's
        assert_eq!(p.claim(0x1234, C2, 0x9733, touch::CONTACT, 2_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(0x1234, C2, 0x9733, C1, true, 10.0, 2_100)), TransformVerdict::Accepted);
        p.host_changed(CELL, C1, 9_000);
        assert_eq!(p.rows().iter().find(|(refr, _)| *refr == 0x1234).map(|(_, r)| r.owner), Some(C2));
    }

    #[test]
    fn a_departed_players_rows_fall_back_to_the_host_epochs_kept() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        p.gone(C2);
        assert_eq!(p.owner_of(BASKET, C1, 1_100), C1);
        assert_eq!(p.owner_of(BASKET, 0, 1_100), 0);
        assert_eq!(p.epoch_of(BASKET), 1);
        // the host's samples pass; c2's return touch is a grant at epoch 2
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_200)), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_300), ClaimVerdict::Granted(2));
    }

    #[test]
    fn an_awake_row_lapses_in_silence_and_a_touch_takes_it_back() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_100)), TransformVerdict::Accepted);
        // the toucher stops streaming awake: the host simulates again, a late sample is refused
        assert_eq!(p.owner_of(BASKET, C1, 1_100 + PROP_SILENCE_MS - 1), C2);
        assert_eq!(p.owner_of(BASKET, C1, 1_100 + PROP_SILENCE_MS), C1);
        assert_eq!(p.live_owner(BASKET, 1_100 + PROP_SILENCE_MS), 0);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_100 + PROP_SILENCE_MS)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_100 + PROP_SILENCE_MS)), TransformVerdict::Accepted);
        // a touch takes it back at the next epoch
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 9_000), ClaimVerdict::Granted(2));
        assert_eq!(p.live_owner(BASKET, 9_000), C2);
    }

    #[test]
    fn a_grab_holds_through_stillness_and_silence_until_the_drop_or_its_limit() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        // held still in the hand: an asleep sample starts no lease, silence does not lapse it
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 1_500)), TransformVerdict::Accepted);
        assert_eq!(p.owner_of(BASKET, C1, 1_500 + PROP_LEASE_MS + PROP_SILENCE_MS), C2);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 9_000), ClaimVerdict::Held);
        // the drop wakes the row for the dropper, whose fall samples pass; its rest starts the lease
        assert_eq!(p.claim(BASKET, C2, CELL, touch::DROP, 9_100), ClaimVerdict::Same);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 9_200)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 9_300)), TransformVerdict::Accepted);
        assert_eq!(p.owner_of(BASKET, C1, 9_300 + PROP_LEASE_MS - 1), C2);
        assert_eq!(p.owner_of(BASKET, C1, 9_300 + PROP_LEASE_MS), C1);
        // a grab nobody releases ends at its limit; the host's takeover clears the grab
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.owner_of(BASKET, C1, 1_000 + PROP_GRAB_MAX_MS - 1), C2);
        assert_eq!(q.owner_of(BASKET, C1, 1_000 + PROP_GRAB_MAX_MS), C1);
        assert_eq!(q.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_000 + PROP_GRAB_MAX_MS)), TransformVerdict::Accepted);
        assert_eq!(q.claim(BASKET, C3, CELL, touch::CONTACT, 1_100 + PROP_GRAB_MAX_MS), ClaimVerdict::Granted(2));
    }

    // thuum D-S3 point 4: Embershard Mine 0xb6be6's falling rocks (0xb6d45 and
    // its six siblings, thuum docs/research/trap-census-2026-11-05.md row 18)
    const MINE: u32 = 0xb6be6;
    const ROCK: u32 = 0xb6d45;

    #[test]
    fn a_trap_bodys_first_firing_wins_while_it_moves() {
        let mut p = Props::default();
        // c2's firing set the rock moving: its impulse claim is granted
        assert_eq!(p.claim_world_body(ROCK, C2, MINE, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.owner_of(ROCK, C1, 1_000), C2);
        // c1's own copy of the same firing: refused while the body moves,
        // before and after c2's first awake sample, the epoch unmoved
        assert_eq!(p.claim_world_body(ROCK, C1, MINE, 1_050), ClaimVerdict::Moving);
        assert_eq!(p.transform(&facts(ROCK, C2, MINE, C1, false, 40.0, 1_100)), TransformVerdict::Accepted);
        assert_eq!(p.claim_world_body(ROCK, C1, MINE, 1_150), ClaimVerdict::Moving);
        assert_eq!(p.claim_world_body(ROCK, C3, MINE, 1_150), ClaimVerdict::Moving);
        assert_eq!(p.owner_of(ROCK, C1, 1_150), C2);
        assert_eq!(p.epoch_of(ROCK), 1);
        // the firing seat's own claim again is Same
        assert_eq!(p.claim_world_body(ROCK, C2, MINE, 1_200), ClaimVerdict::Same);
        // the rest sample: the body sleeps, the lease runs, and a new firing
        // takes the resting body at the next epoch
        assert_eq!(p.transform(&facts(ROCK, C2, MINE, C1, true, 40.0, 1_300)), TransformVerdict::Accepted);
        assert_eq!(p.claim_world_body(ROCK, C1, MINE, 1_400), ClaimVerdict::Granted(2));
        assert_eq!(p.owner_of(ROCK, C2, 1_400), C1);
    }

    #[test]
    fn a_trap_bodys_row_lapsed_in_silence_is_any_firings() {
        let mut p = Props::default();
        assert_eq!(p.claim_world_body(ROCK, C2, MINE, 1_000), ClaimVerdict::Granted(1));
        // c2 never streams: the row lapses, and c1's firing takes the body
        assert_eq!(p.claim_world_body(ROCK, C1, MINE, 1_000 + PROP_SILENCE_MS - 1), ClaimVerdict::Moving);
        assert_eq!(p.claim_world_body(ROCK, C1, MINE, 1_000 + PROP_SILENCE_MS), ClaimVerdict::Granted(2));
    }

    #[test]
    fn an_items_impulse_still_takes_it_from_another_seat() {
        // ADR-058's newest touch wins stands for items: a shout's impulse on a
        // basket another seat kicked is granted while it flies
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_100)), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::IMPULSE, 1_150), ClaimVerdict::Granted(2));
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
        // the host's first sample of an unclaimed prop: a new row's full
        // allowance, which the step spends
        let mut q = Props::default();
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS + 1.0, 0)), TransformVerdict::TooFast);
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, 200.0, 0)), TransformVerdict::Accepted);
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
        assert_eq!(r.claim_world_body(ROCK, C2, MINE, 1_000), ClaimVerdict::Granted(1));
        for i in 1..=960_u64 {
            assert_eq!(r.transform(&facts(ROCK, C2, MINE, C1, false, 50.0, 1_000 + i * 1_000 / 96)), TransformVerdict::Accepted);
        }
    }

    #[test]
    fn a_grant_leaves_the_rows_allowance_as_it_stands() {
        // the allowance is the prop's, refilled by time alone: seats trading
        // a prop by claims cannot each start with a full one
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, PROP_STEP_ALLOWANCE_UNITS, 1_000)), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C3, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(2));
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
    }
}
