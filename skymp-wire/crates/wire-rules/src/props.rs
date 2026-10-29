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
}

/// Every prop's row and every prop's epoch (kept after its row is gone).
#[derive(Debug, Default)]
pub struct Props {
    rows: HashMap<u32, PropRow>,
    epochs: HashMap<u32, u32>,
}

impl PropRow {
    /// Whether the row names its owner now: awake, or asleep inside the
    /// lease; an owner of 0 never does.
    fn live(&self, now_ms: u64) -> bool {
        self.owner != 0 && (!self.asleep || self.lease_until_ms.is_some_and(|until| now_ms < until))
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
            PropRow { owner: player, epoch, cell, lease_until_ms: None, asleep: false, grabbed: touch == touch::GRAB },
        );
        ClaimVerdict::Granted(epoch)
    }

    /// `player` reports `refr`'s transform, landing in `cell` (0 for none)
    /// whose host is `cell_host` (0 while nobody), the body `asleep` or not,
    /// at `now_ms`: taken from the prop's owner alone (`owner_of`); a sleep
    /// starts the lease, a wake ends it; a host's first sample of an
    /// unclaimed prop writes its row at the kept epoch.
    pub fn transform(&mut self, refr: u32, player: u32, cell: u32, cell_host: u32, asleep: bool, now_ms: u64) -> TransformVerdict {
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
        let epoch = self.epoch_of(refr);
        let row = self.rows.entry(refr).or_insert(PropRow {
            owner,
            epoch,
            cell,
            lease_until_ms: None,
            asleep: false,
            grabbed: false,
        });
        row.owner = owner;
        row.cell = cell;
        row.asleep = asleep;
        row.lease_until_ms = asleep.then(|| now_ms.saturating_add(PROP_LEASE_MS));
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
        assert_eq!(p.transform(BASKET, C1, CELL, C1, false, 500), TransformVerdict::Accepted);
        assert_eq!(p.transform(BASKET, C2, CELL, C1, false, 600), TransformVerdict::NotOwner);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(BASKET, C1, CELL, C1, false, 1_100), TransformVerdict::NotOwner);
        assert_eq!(p.transform(BASKET, C2, CELL, C1, false, 1_100), TransformVerdict::Accepted);
        // the body sleeps: the toucher keeps it for the lease, then the host
        assert_eq!(p.transform(BASKET, C2, CELL, C1, true, 2_000), TransformVerdict::Accepted);
        assert_eq!(p.owner_of(BASKET, C1, 2_000 + PROP_LEASE_MS - 1), C2);
        assert_eq!(p.owner_of(BASKET, C1, 2_000 + PROP_LEASE_MS), C1);
        assert_eq!(p.transform(BASKET, C2, CELL, C1, false, 2_000 + PROP_LEASE_MS), TransformVerdict::NotOwner);
        assert_eq!(p.transform(BASKET, C1, CELL, C1, false, 2_000 + PROP_LEASE_MS), TransformVerdict::Accepted);
        // a wake inside the lease ends it
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.transform(BASKET, C2, CELL, C1, true, 2_000), TransformVerdict::Accepted);
        assert_eq!(q.transform(BASKET, C2, CELL, C1, false, 2_500), TransformVerdict::Accepted);
        assert_eq!(q.owner_of(BASKET, C1, 9_000), C2);
        // a lapsed toucher's new touch is a grant again (the host's seat must
        // hear the word), at the next epoch
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 6_000), ClaimVerdict::Granted(2));
    }

    #[test]
    fn a_prop_in_no_cell_or_an_unhosted_unclaimed_cell_freezes() {
        let mut p = Props::default();
        assert_eq!(p.transform(BASKET, C1, 0, C1, false, 0), TransformVerdict::WrongCell);
        assert_eq!(p.transform(BASKET, C1, CELL, 0, false, 0), TransformVerdict::WrongCell);
        // a live toucher keeps simulating in a cell whose host left
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(BASKET, C1, CELL, 0, false, 1_100), TransformVerdict::Accepted);
        assert_eq!(p.transform(BASKET, C1, CELL, 0, true, 1_200), TransformVerdict::Accepted);
        assert_eq!(p.transform(BASKET, C1, CELL, 0, false, 1_200 + PROP_LEASE_MS), TransformVerdict::WrongCell);
    }

    #[test]
    fn a_host_change_takes_the_lapsed_rows_and_leaves_the_live_ones_epochs_kept() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(BASKET, C2, CELL, C1, true, 1_500), TransformVerdict::Accepted);
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
        assert_eq!(p.transform(0x1234, C2, 0x9733, C1, true, 2_100), TransformVerdict::Accepted);
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
        assert_eq!(p.transform(BASKET, C1, CELL, C1, false, 1_200), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_300), ClaimVerdict::Granted(2));
    }
}
