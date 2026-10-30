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

/// The step budget of one sample against the record: `PROP_MAX_SPEED_UNITS_PER_S`
/// for the time since the last accepted sample, never less than
/// `PROP_STEP_FLOOR_UNITS` (the first sample after a grant, jitter). A sample
/// over the budget is refused and leaves the record where it stood: a seat
/// cannot carry a prop across the world in one message, and no owner moves
/// it faster than a kicked body flies. UNMEASURED: x-prop-push sizes both.
pub const PROP_MAX_SPEED_UNITS_PER_S: f32 = 6_000.0;
/// The least a sample may move the record without a time base (see
/// `PROP_MAX_SPEED_UNITS_PER_S`).
pub const PROP_STEP_FLOOR_UNITS: f32 = 128.0;

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
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
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
    /// silence clock and the step budget's base.
    pub last_sample_ms: u64,
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
    /// (or not a number): refused, the record stands.
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
            },
        );
        ClaimVerdict::Granted(epoch)
    }

    /// `player` reports `refr`'s transform, landing in `cell` (0 for none)
    /// whose record's cell is hosted by `cell_host` (0 while nobody), the
    /// body `asleep` or not, `moved` units from the record, at `now_ms`:
    /// taken from the prop's owner alone (`owner_of`) and inside the step
    /// budget; a sleep starts the lease (a grabbed row's does not), a wake
    /// ends it; a host's first sample of an unclaimed prop writes its row at
    /// the kept epoch, and a host's sample of a lapsed row takes it over,
    /// ending a grab its holder never released.
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
        let allowed = match self.rows.get(&refr) {
            Some(row) if row.live(now_ms) && row.owner == player => {
                // the time base, clamped to ten seconds: a long silence has
                // lapsed the row already, a short one buys a short step
                let elapsed_ms = u16::try_from(now_ms.saturating_sub(row.last_sample_ms).min(10_000)).unwrap_or(10_000);
                (PROP_MAX_SPEED_UNITS_PER_S * f32::from(elapsed_ms) / 1_000.0).max(PROP_STEP_FLOOR_UNITS)
            }
            _ => PROP_STEP_FLOOR_UNITS,
        };
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

    #[test]
    fn a_sample_farther_than_the_step_budget_is_refused_and_leaves_the_record() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        // the first sample after the grant: the floor
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, PROP_STEP_FLOOR_UNITS + 1.0, 1_010)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, PROP_STEP_FLOOR_UNITS, 1_010)), TransformVerdict::Accepted);
        // 100 ms later: 600 units at 6000 a second
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 601.0, 1_110)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 600.0, 1_110)), TransformVerdict::Accepted);
        // a refused sample moves no clock: the next is judged from the last accepted
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 5_000.0, 1_200)), TransformVerdict::TooFast);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 539.0, 1_200)), TransformVerdict::Accepted);
        // not a number: refused
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, f32::NAN, 1_300)), TransformVerdict::TooFast);
        // the host's first sample of an unclaimed prop: the floor
        let mut q = Props::default();
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, 200.0, 0)), TransformVerdict::TooFast);
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, 50.0, 0)), TransformVerdict::Accepted);
    }
}
