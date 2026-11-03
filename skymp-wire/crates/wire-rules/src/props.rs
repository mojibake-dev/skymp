//! Physics props (thuum docs/verbs/prop-physics.md, ADR-058; the arbitration
//! rule of ADR-063 decision 4, 2026-11-03): one simulating seat per prop,
//! Garry's Mod's shape without its server. A prop (a kicked basket, a dropped
//! sword) is simulated by exactly one seat at a time: the cell's host by
//! default, the seat whose player touched it while it moves, a grabber until
//! its release. The server decides who simulates (the claim, R0) and records
//! the simulator's transforms (R2: only the owner's are taken). Ownership
//! changes by the server's word alone (`PropOwner`): a grant, the return to
//! the host at the owner's own asleep sample (player 0, the host default),
//! the departure's return. No timer hands a prop back (playtest 41, Eli:
//! "item physics sync is more broken than it was when we STARTED testing":
//! the 3 s lease and the 3 s silence returned props the owner still streamed,
//! and no seat heard it); the one timer left is the grab's cap. The claim's
//! rows: a grab beats a touch and nothing beats a grab (Held); a contact, a
//! drop or an impulse never takes a prop another seat streams awake
//! (Moving); anything at rest or stalled is the newest touch's. The owner
//! table is ADR-050 decision 4's second row kind, "prop", beside the cells
//! rule's follows, margins and claims (`cells.rs`); the cell is still the
//! policy: a prop in a cell nobody hosts and nobody holds freezes at the
//! record. The core gathers the facts (who hosts the prop's cell, where the
//! record stands, the wall clock) and asks here; nothing here knows a
//! position.

use std::collections::{HashMap, HashSet};

/// How recent the owner's last accepted awake sample must be for the prop to
/// count as MOVING, which a contact, a drop or an impulse from another seat
/// cannot take (ADR-063 decision 4's arbitration, 2026-11-03: a non-owner's
/// contact never takes a moving prop). An owner streams at most every 100 ms
/// while the body moves and sends one asleep sample at rest, so a row this
/// quiet is at rest, stalled (a stream that died without its asleep sample)
/// or its samples are not getting through; any of these is the next touch's.
/// The same length as the client's STREAM_LIVE_MS. UNMEASURED.
pub const PROP_MOVING_MS: u64 = 1_000;

/// The longest a grab holds a prop against every other claim without the
/// holder's drop: a safety against a release the holder's seat never
/// reported, the one timer the table keeps. A grabbed row never rests (a held
/// object stands still in the hand); past the cap the table's tick returns
/// it to the host with a word (`tick`), or the holder's own next sample does
/// (Capped), whichever comes first.
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
/// The budget of a simulator's FIRST sample of a prop with no live row (the
/// host's sample of a prop nobody touched, or a lapsed row's): half a second
/// of flight. Playtest 41 (2026-11-02, Eli: "just blast a bunch of cabbages
/// ... only some stayed synced"): a shout's blast moved a cabbage farther
/// than the floor before the host's first sample, the sample was refused
/// and no row was written, so every later sample was measured against the
/// never-moving record and the prop never synced again. The row is written
/// at the first accepted sample and the elapsed budget takes over.
pub const PROP_FIRST_SAMPLE_UNITS: f32 = PROP_MAX_SPEED_UNITS_PER_S / 2.0;

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
    /// The simulating player's actor form id; 0 once the prop went back to
    /// the host (at rest, or the owner left): the cell's host simulates,
    /// `owner_of`.
    pub owner: u32,
    /// The prop's owner epoch: up at every word (a grant, a return to the
    /// host), the wire's `PropOwner.epoch`.
    pub epoch: u32,
    /// The cell the prop's record stands in, by the last claim or accepted
    /// transform.
    pub cell: u32,
    /// The body's last reported state; a resting row names no simulator (the
    /// host's by default).
    pub asleep: bool,
    /// The owner holds the prop in its hands (a grab since its last claim,
    /// no drop yet): another player's claim is refused as Held. HYPOTHESIS:
    /// the engine's grab is exclusive on the grabbing seat, so a contact
    /// from another seat cannot take it without breaking the grab there.
    pub grabbed: bool,
    /// When this owner took the row (the grant, or the host's first sample):
    /// a grab's clock.
    pub since_ms: u64,
    /// The last accepted sample's time (the grant's before any): the moving
    /// window and the step budget's base.
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
    /// Another seat streams the prop awake right now and the claimant's
    /// touch is no grab (ADR-063 decision 4: a non-owner's contact never
    /// takes a moving prop); nothing changes, the claimant's copy stays on
    /// the stream.
    Moving,
    /// The claimant's game reported the prop's cell unloaded and has not
    /// reported it loaded since, nor had the cell back in its reach
    /// (`unloaded`, `loaded`, `reached`): its 3D of the prop is gone, so a
    /// claim from it there is a late one; nothing changes.
    Away,
}

/// The answer to a simulator's transform (ADR-058 decision 2).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransformVerdict {
    /// The sender simulates the prop: the record takes the transform.
    Accepted,
    /// Another seat simulates it (the owner, or the cell's host by default).
    NotOwner,
    /// The prop stands in no cell at all, or in a cell nobody hosts while
    /// no live row holds it: it freezes at the record.
    WrongCell,
    /// The sample is farther from the record than the step budget allows
    /// (or not a number): refused, the record stands.
    TooFast,
    /// Taken, and the body's rest returned the prop to the cell's host at
    /// this epoch, the host's own rest included: the word `PropOwner` with
    /// player 0 goes to every listener (ADR-063 decision 4: ownership
    /// returns to the host only at rest on the owner's screen, by the
    /// owner's own final sample, and every return is a word, so no seat is
    /// left holding an older word).
    Returned(u32),
    /// Refused: the sender held the prop grabbed past PROP_GRAB_MAX_MS and
    /// this sample is the first the table sees after the cap; the prop is the
    /// host's again at this epoch and the word goes out (the cap ends by a
    /// word, never by a silent drop).
    Capped(u32),
    /// Refused as farther than the step budget allows, but the sample said
    /// asleep: the rest is taken (the prop is the host's again at this
    /// epoch, the word goes out), the position is not (the record stands);
    /// without it a refused final sample left a live row nobody could move.
    RestRefused(u32),
    /// Woken by the host: its awake sample of a prop that was not its live
    /// row (a resting prop its engine moved, a grab past its cap): the row is
    /// the host's at this new epoch and the word names the host BY ID, so
    /// every seat's word names the seat that streams and a host change
    /// leaves the old host streaming until its rest (the review of
    /// 2026-11-03: a word of player 0 over a row naming the host stranded the
    /// prop after a host change, nobody simulating it).
    Woken(u32),
    /// A wake by a sender whose game reported the sample's cell unloaded
    /// and has not reported it loaded since, nor had the cell back in its
    /// reach: an unloading host still in its grace waking the prop it just
    /// left (the review of 2026-11-03); a live owner's own stream passes;
    /// nothing changes.
    Away,
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
    /// gate on the seat's own report, not on a position.
    away: HashSet<(u32, u32)>,
}

impl PropRow {
    /// Whether the row names its owner now: grabbed inside the grab's cap, or
    /// awake. A resting row and an owner of 0 never do (the host's by
    /// default); no silence lapses a row.
    fn live(&self, now_ms: u64) -> bool {
        if self.owner == 0 {
            return false;
        }
        if self.grabbed {
            return now_ms < self.since_ms.saturating_add(PROP_GRAB_MAX_MS);
        }
        !self.asleep
    }

    /// Whether another seat streams the prop awake right now: live, not
    /// grabbed, the last accepted sample inside the moving window.
    fn moving(&self, now_ms: u64) -> bool {
        self.live(now_ms) && !self.grabbed && now_ms < self.last_sample_ms.saturating_add(PROP_MOVING_MS)
    }
}

impl Props {
    /// Who simulates `refr` at `now_ms`, given that `cell_host` hosts its
    /// cell (0 while nobody does): the row's owner while the body is awake
    /// or held, else the host.
    #[must_use]
    pub fn owner_of(&self, refr: u32, cell_host: u32, now_ms: u64) -> u32 {
        match self.rows.get(&refr) {
            Some(row) if row.live(now_ms) => row.owner,
            _ => cell_host,
        }
    }

    /// The prop's owner epoch: the last word's, 0 before any.
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
    /// `now_ms` (ADR-063 decision 4's arbitration): the claimant that
    /// simulates it already keeps it (Same, the grab mark from the touch);
    /// another player's grab holds it against everything (Held); a prop
    /// another seat streams awake is nobody else's by a contact, a drop or an
    /// impulse (Moving), a grab takes it (the grabber's engine has the body
    /// in hand); a prop at rest, stalled or unclaimed is the newest touch's
    /// (Granted, the epoch up, the word to every listener). The claimant's
    /// cell and distance are no gate (ADR-063).
    pub fn claim(&mut self, refr: u32, player: u32, cell: u32, touch: u8, now_ms: u64) -> ClaimVerdict {
        if self.away.contains(&(player, cell)) {
            return ClaimVerdict::Away;
        }
        if let Some(row) = self.rows.get_mut(&refr) {
            if row.live(now_ms) {
                if row.owner == player {
                    // a grab restarts the cap's clock (the host's grab of a
                    // prop it has streamed for a while is still a fresh hold)
                    if touch == touch::GRAB && !row.grabbed {
                        row.since_ms = now_ms;
                    }
                    row.grabbed = touch == touch::GRAB;
                    row.cell = cell;
                    // a touch wakes the row: the holder's drop is followed by
                    // its fall samples
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
        self.rows.insert(
            refr,
            PropRow {
                owner: player,
                epoch,
                cell,
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
    /// budget. Every asleep sample of an ungrabbed row returns the prop to
    /// the host at once, the epoch up (Returned: the core sends the word with
    /// player 0), the host's own rest included; a grabbed row's rest returns
    /// nothing (a held object stands still); a host's awake sample of a prop
    /// that is not its live row wakes it (Woken: the row the host's at a new
    /// epoch, the word naming the host by id).
    pub fn transform(&mut self, f: &SampleFacts) -> TransformVerdict {
        let SampleFacts { refr, player, cell, cell_host, asleep, moved, now_ms } = *f;
        if cell == 0 {
            return TransformVerdict::WrongCell;
        }
        // the grab's cap, noticed at the holder's first sample past it: the
        // prop is the host's again by a word, the sample refused
        if let Some(row) = self.rows.get_mut(&refr) {
            if row.owner == player && row.grabbed && now_ms >= row.since_ms.saturating_add(PROP_GRAB_MAX_MS) {
                row.owner = 0;
                row.grabbed = false;
                row.asleep = true;
                let next = row.epoch.saturating_add(1);
                row.epoch = next;
                self.epochs.insert(refr, next);
                return TransformVerdict::Capped(next);
            }
        }
        let owner = self.owner_of(refr, cell_host, now_ms);
        if owner == 0 {
            return TransformVerdict::WrongCell;
        }
        if owner != player {
            return TransformVerdict::NotOwner;
        }
        // the unload's state gates a WAKE alone: a live owner streaming its
        // own row into a cell its game reported unloaded (a missed load
        // report) keeps streaming, its row never stranded; a host's sample
        // that would take a resting row there waits for the load report or
        // the cell's return to its reach
        let own_live = self.rows.get(&refr).is_some_and(|r| r.live(now_ms) && r.owner == player);
        if !own_live && self.away.contains(&(player, cell)) {
            return TransformVerdict::Away;
        }
        let allowed = match self.rows.get(&refr) {
            Some(row) if row.live(now_ms) && row.owner == player => {
                // the time base, clamped to ten seconds (a stalled stream's
                // next sample buys a long step, never a teleport)
                let elapsed_ms = u16::try_from(now_ms.saturating_sub(row.last_sample_ms).min(10_000)).unwrap_or(10_000);
                (PROP_MAX_SPEED_UNITS_PER_S * f32::from(elapsed_ms) / 1_000.0).max(PROP_STEP_FLOOR_UNITS)
            }
            // no live row of this simulator's: the host's first sample of a
            // prop at rest, which may have flown half a second already (a
            // blast)
            _ => PROP_FIRST_SAMPLE_UNITS,
        };
        if !moved.is_finite() || moved > allowed {
            if asleep && !self.rows.get(&refr).is_some_and(|r| r.grabbed) {
                // a refused final sample still ends the owner's turn: the
                // record stands where it was, the prop is the host's again
                let next = self.epoch_of(refr).saturating_add(1);
                self.epochs.insert(refr, next);
                let row = self.rows.entry(refr).or_insert(PropRow {
                    owner: 0,
                    epoch: next,
                    cell,
                    asleep: true,
                    grabbed: false,
                    since_ms: now_ms,
                    last_sample_ms: now_ms,
                });
                row.owner = 0;
                row.asleep = true;
                row.epoch = next;
                return TransformVerdict::RestRefused(next);
            }
            return TransformVerdict::TooFast;
        }
        let epoch = self.epoch_of(refr);
        // the host's sample of a prop that is not its live row (no row, a
        // resting one, a grab past its cap): the host's row now, the grab
        // mark gone, and an awake sample is a wake (a word by id below)
        let woke = !self.rows.get(&refr).is_some_and(|r| r.live(now_ms) && r.owner == owner);
        let row = self.rows.entry(refr).or_insert(PropRow {
            owner,
            epoch,
            cell,
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
        if asleep && !row.grabbed {
            // the body rests: the prop is the host's again by this very
            // sample, the host's own rest included, and every listener hears
            // it (a seat holding an older word would otherwise keep it)
            row.owner = 0;
            let next = epoch.saturating_add(1);
            row.epoch = next;
            self.epochs.insert(refr, next);
            return TransformVerdict::Returned(next);
        }
        if woke {
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

    /// `host` now hosts `cell` (a grant): no row changes. A resting row
    /// names nobody already (`owner_of` names the new host), a live
    /// toucher's row survives the grant as the cells rule's claims do, so
    /// does the old host's live row (its wake word named it by id, so it
    /// streams until its rest), and a grab past its cap waits for the
    /// tick's word (clearing it here left the followers holding the
    /// holder's id with no word, the review of 2026-11-03). The entry stays
    /// so the core's grant path names the moment.
    pub fn host_changed(&mut self, _cell: u32, _host: u32, _now_ms: u64) {}

    /// `player`'s game unloaded `cell` (a door, a teleport, a square
    /// leaving its grid): its 3D of every prop there is gone, and the
    /// client's rescan untracks the prop before any final sample
    /// (HYPOTHESIS: the client's order, until a run), so every row of its in
    /// the cell (live or past its cap) falls back to the host by default at
    /// a new epoch, and the core sends the word (player 0) for each, as a
    /// departure's would; a resting row names nobody already. Returns
    /// (refr, epoch) per returned prop, sorted.
    pub fn unloaded(&mut self, player: u32, cell: u32, _now_ms: u64) -> Vec<(u32, u32)> {
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

    /// `player` left the server: every row still naming it (live or past
    /// its cap) falls back to its cell's host at a new epoch, and the core
    /// sends the word (player 0) for each; a resting row of its was the
    /// host's already (its rest's word said so). Returns (refr, epoch) per
    /// returned prop, sorted.
    pub fn gone(&mut self, player: u32, _now_ms: u64) -> Vec<(u32, u32)> {
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

    /// The table's clock, once a second from the core: every grab past its
    /// cap returns to its cell's host at a new epoch, and the core sends the
    /// word (player 0) for each (the cap ends by a word, whether or not the
    /// holder ever samples again). Returns (refr, epoch) per returned prop.
    pub fn tick(&mut self, now_ms: u64) -> Vec<(u32, u32)> {
        let mut returned = Vec::new();
        for (&refr, row) in self.rows.iter_mut().filter(|(_, r)| r.owner != 0 && r.grabbed) {
            if now_ms >= row.since_ms.saturating_add(PROP_GRAB_MAX_MS) {
                row.owner = 0;
                row.grabbed = false;
                row.asleep = true;
                let next = row.epoch.saturating_add(1);
                row.epoch = next;
                returned.push((refr, next));
            }
        }
        for &(refr, epoch) in &returned {
            self.epochs.insert(refr, epoch);
        }
        returned.sort_unstable();
        returned
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
    fn a_touch_at_rest_is_granted_the_same_toucher_keeps_it_and_a_moving_prop_is_nobody_elses_by_a_touch() {
        // ADR-063 decision 4 (2026-11-03): a non-owner's contact never takes a
        // moving prop; at rest or stalled the newest touch wins
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.owner_of(BASKET, C1, 1_000), C2);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_100), ClaimVerdict::Same);
        assert_eq!(p.epoch_of(BASKET), 1);
        // c1 walks into it while c2 streams it (the grant counts as a sample): refused
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_200), ClaimVerdict::Moving);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_500)), TransformVerdict::Accepted);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::IMPULSE, 1_600), ClaimVerdict::Moving);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::DROP, 1_700), ClaimVerdict::Moving);
        assert_eq!(p.owner_of(BASKET, C1, 1_700), C2);
        assert_eq!(p.epoch_of(BASKET), 1);
        // the stream dies without its asleep sample: past the moving window the next touch takes it
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_500 + PROP_MOVING_MS - 1), ClaimVerdict::Moving);
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_500 + PROP_MOVING_MS), ClaimVerdict::Granted(2));
        assert_eq!(p.owner_of(BASKET, C1, 1_500 + PROP_MOVING_MS), C1);
        // another prop counts its own epochs
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 3_000), ClaimVerdict::Granted(1));
        assert_eq!(p.epoch_of(BASKET), 2);
    }

    #[test]
    fn a_grab_beats_a_touch_and_nothing_beats_a_grab() {
        let mut p = Props::default();
        // c2 streams it awake; c1's grab takes it (the grabber's engine has the body in hand)
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.claim(BASKET, C1, CELL, touch::GRAB, 1_100), ClaimVerdict::Granted(2));
        assert_eq!(p.owner_of(BASKET, C2, 1_100), C1);
        // held: every other seat's touch, grab included, is Held
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_200), ClaimVerdict::Held);
        assert_eq!(p.claim(BASKET, C2, CELL, touch::IMPULSE, 1_300), ClaimVerdict::Held);
        assert_eq!(p.claim(BASKET, C3, CELL, touch::GRAB, 1_400), ClaimVerdict::Held);
        // held still in the hand: an asleep sample returns nothing
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, true, 10.0, 1_500)), TransformVerdict::Accepted);
        assert_eq!(p.owner_of(BASKET, C2, 9_000), C1);
        // the holder's drop clears the grab; the fall streams, the rest returns it to the host with the word
        assert_eq!(p.claim(BASKET, C1, CELL, touch::DROP, 9_100), ClaimVerdict::Same);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, false, 10.0, 9_200)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, true, 10.0, 9_300)), TransformVerdict::Returned(3));
        assert_eq!(p.owner_of(BASKET, C2, 9_300), C2);
        assert_eq!(p.epoch_of(BASKET), 3);
        // a grab nobody releases ends at its cap, by a word: the holder's first sample past it is refused as
        // Capped and the prop is the host's at the next epoch; the next touch takes it
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.owner_of(BASKET, C1, 1_000 + PROP_GRAB_MAX_MS - 1), C2);
        assert_eq!(q.owner_of(BASKET, C1, 1_000 + PROP_GRAB_MAX_MS), C1);
        assert_eq!(q.tick(1_000 + PROP_GRAB_MAX_MS - 1), vec![]);
        assert_eq!(q.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_000 + PROP_GRAB_MAX_MS)), TransformVerdict::Capped(2));
        assert_eq!(q.epoch_of(BASKET), 2);
        assert_eq!(q.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_100 + PROP_GRAB_MAX_MS)), TransformVerdict::NotOwner);
        assert_eq!(q.claim(BASKET, C3, CELL, touch::CONTACT, 1_200 + PROP_GRAB_MAX_MS), ClaimVerdict::Granted(3));
        // or the table's tick returns it first, whether or not the holder samples again
        let mut t = Props::default();
        assert_eq!(t.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(t.tick(1_000 + PROP_GRAB_MAX_MS), vec![(BASKET, 2)]);
        assert_eq!(t.owner_of(BASKET, C1, 1_000 + PROP_GRAB_MAX_MS), C1);
        assert_eq!(t.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_100 + PROP_GRAB_MAX_MS)), TransformVerdict::NotOwner);
        // and a holder that leaves after a silent cap is named by nobody: its departure words the row too
        let mut u = Props::default();
        assert_eq!(u.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(u.gone(C2, 2_000 + PROP_GRAB_MAX_MS), vec![(BASKET, 2)]);
        // a grab on a prop the holder already streamed restarts the cap's clock (the host's own grab)
        let mut r = Props::default();
        assert_eq!(r.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(r.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 60_000)), TransformVerdict::Accepted);
        assert_eq!(r.claim(BASKET, C2, CELL, touch::GRAB, 60_100), ClaimVerdict::Same);
        assert_eq!(r.owner_of(BASKET, C1, 60_100 + PROP_GRAB_MAX_MS - 1), C2);
        assert_eq!(r.owner_of(BASKET, C1, 60_100 + PROP_GRAB_MAX_MS), C1);
    }

    #[test]
    fn only_the_owner_moves_the_record_and_its_rest_returns_the_prop_to_the_host_as_a_word() {
        let mut p = Props::default();
        // the host's own sample of an unclaimed prop wakes it: the row the host's at epoch 1, the word naming the host
        // by id; its next sample is the live row's; its rest returns it with a word too (player 0, epoch 2)
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 500)), TransformVerdict::Woken(1));
        assert_eq!(p.live_owner(BASKET, 500), C1);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 550)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 600)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, true, 10.0, 700)), TransformVerdict::Returned(2));
        assert_eq!(p.epoch_of(BASKET), 2);
        assert_eq!(p.owner_of(BASKET, C1, 700), C1);
        // c2's touch at rest: granted; only c2's samples pass
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(3));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_100)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_100)), TransformVerdict::Accepted);
        // no timer hands it back: a long quiet keeps c2 the owner, the host's own sample refused as any non-owner's
        assert_eq!(p.owner_of(BASKET, C1, 60_000), C2);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 60_000)), TransformVerdict::NotOwner);
        // c2's own asleep sample: the host's again at once, at the next epoch, and the word goes out
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 61_000)), TransformVerdict::Returned(4));
        assert_eq!(p.owner_of(BASKET, C1, 61_000), C1);
        assert_eq!(p.live_owner(BASKET, 61_000), 0);
        assert_eq!(p.epoch_of(BASKET), 4);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 61_100)), TransformVerdict::NotOwner);
        // the host's kick of the resting prop is a wake, by id (epoch 5); its rest returns it again (6); c2's new touch
        // at rest is a grant at 7
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 61_100)), TransformVerdict::Woken(5));
        assert_eq!(p.live_owner(BASKET, 61_100), C1);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, true, 10.0, 61_200)), TransformVerdict::Returned(6));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 62_000), ClaimVerdict::Granted(7));
    }

    #[test]
    fn a_prop_in_no_cell_or_an_unhosted_unclaimed_cell_freezes() {
        let mut p = Props::default();
        assert_eq!(p.transform(&facts(BASKET, C1, 0, C1, false, 10.0, 0)), TransformVerdict::WrongCell);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, false, 10.0, 0)), TransformVerdict::WrongCell);
        // a live toucher keeps simulating in a cell whose host left; its rest returns the prop to nobody
        assert_eq!(p.claim(BASKET, C1, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, false, 10.0, 1_100)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, true, 10.0, 1_200)), TransformVerdict::Returned(2));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, 0, false, 10.0, 1_300)), TransformVerdict::WrongCell);
    }

    #[test]
    fn a_host_change_leaves_the_live_rows_and_the_resting_ones_are_the_new_hosts_epochs_kept() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 10.0, 1_500)), TransformVerdict::Returned(2));
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 1_600), ClaimVerdict::Granted(1));
        // c3 takes the cell: the resting basket is its by default, the live cabbage stays c2's
        p.host_changed(CELL, C3, 2_000);
        assert_eq!(p.owner_of(BASKET, C3, 2_000), C3);
        assert_eq!(p.owner_of(0x54a44, C3, 2_000), C2);
        assert_eq!(p.epoch_of(BASKET), 2);
        let rows = p.rows();
        assert_eq!(rows.len(), 2);
        assert!(rows.iter().any(|(refr, row)| *refr == BASKET && row.owner == 0 && row.epoch == 2));
        // a row in another cell is not the grant's
        assert_eq!(p.claim(0x1234, C2, 0x9733, touch::CONTACT, 2_000), ClaimVerdict::Granted(1));
        p.host_changed(CELL, C1, 9_000);
        assert_eq!(p.rows().iter().find(|(refr, _)| *refr == 0x1234).map(|(_, r)| r.owner), Some(C2));
        // a grab past its cap survives the grant and the tick words it (the grant used to clear it with no word)
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        q.host_changed(CELL, C3, 1_000 + PROP_GRAB_MAX_MS + 1);
        assert_eq!(q.owner_of(BASKET, C3, 1_000 + PROP_GRAB_MAX_MS + 1), C3);
        assert_eq!(q.tick(1_000 + PROP_GRAB_MAX_MS + 2), vec![(BASKET, 2)]);
    }

    #[test]
    fn a_players_unload_of_a_cell_returns_its_live_rows_there_with_a_word_each() {
        // the review of 2026-11-03 (D2): the old host's cell unloads mid-flight (a door, a teleport); its 3D of the prop is
        // gone and the rescan untracks it before any final sample, so the row would stay live naming it and nobody would
        // stream; the unload returns its rows there to the host by default with a word each, as a departure does
        let mut p = Props::default();
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_000)), TransformVerdict::Woken(1));
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::GRAB, 1_100), ClaimVerdict::Granted(1));
        // a resting row and another cell's row are not the unload's
        assert_eq!(p.claim(0x1234, C1, CELL, touch::CONTACT, 1_200), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(0x1234, C1, CELL, C1, true, 10.0, 1_300)), TransformVerdict::Returned(2));
        assert_eq!(p.claim(0x5678, C1, 0x9733, touch::CONTACT, 1_400), ClaimVerdict::Granted(1));
        assert_eq!(p.unloaded(C1, CELL, 2_000), vec![(BASKET, 2)]);
        assert_eq!(p.owner_of(BASKET, C2, 2_000), C2);
        assert_eq!(p.live_owner(BASKET, 2_000), 0);
        assert_eq!(p.epoch_of(BASKET), 2);
        assert_eq!(p.live_owner(0x54a44, 2_000), C2);
        assert_eq!(p.live_owner(0x5678, 2_000), C1);
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
        assert_eq!(p.unloaded(C1, CELL, 2_095), vec![(0x5678, 2)]);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 2_096)), TransformVerdict::Away);
        p.loaded(C1, CELL);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 2_097)), TransformVerdict::Woken(5));
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, true, 10.0, 2_098)), TransformVerdict::Returned(6));
        // the holder's unload drops its grab too (a grab carried through a door stays in no hand), and its claim
        // there waits for its load report as well; a departure clears the state
        assert_eq!(p.unloaded(C2, CELL, 2_100), vec![(0x54a44, 2)]);
        assert_eq!(p.live_owner(0x54a44, 2_100), 0);
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::GRAB, 2_150), ClaimVerdict::Away);
        assert_eq!(p.gone(C2, 2_200), vec![]);
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 2_300), ClaimVerdict::Granted(3));
        // the live owner's own stream into a cell it reported unloaded passes (its row is live), a wake there does not
        assert_eq!(p.unloaded(C2, 0x9733, 2_400), vec![]);
        assert_eq!(p.transform(&facts(0x54a44, C2, 0x9733, C1, false, 10.0, 2_410)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(0x54a44, C2, 0x9733, C1, true, 10.0, 2_420)), TransformVerdict::Returned(4));
        // nothing of c3's, and nothing twice
        assert_eq!(p.unloaded(C3, CELL, 2_400), vec![]);
        assert_eq!(p.unloaded(C1, CELL, 2_500), vec![]);
    }

    #[test]
    fn a_departed_players_live_rows_return_to_the_host_with_a_word_each() {
        let mut p = Props::default();
        assert_eq!(p.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(p.claim(0x54a44, C2, CELL, touch::CONTACT, 1_100), ClaimVerdict::Granted(1));
        // a resting row of c2's is the host's already: no word for it
        assert_eq!(p.claim(0x1234, C2, CELL, touch::CONTACT, 1_200), ClaimVerdict::Granted(1));
        assert_eq!(p.transform(&facts(0x1234, C2, CELL, C1, true, 10.0, 1_300)), TransformVerdict::Returned(2));
        assert_eq!(p.gone(C2, 2_000), vec![(BASKET, 2), (0x54a44, 2)]);
        assert_eq!(p.owner_of(BASKET, C1, 2_100), C1);
        assert_eq!(p.owner_of(BASKET, 0, 2_100), 0);
        assert_eq!(p.epoch_of(BASKET), 2);
        assert_eq!(p.epoch_of(0x1234), 2);
        // the host's samples pass and it streams the prop awake (its wake, by id, epoch 3): c2's return touch waits
        // for the host's rest (a word, epoch 4), then is a grant at 5
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 2_200)), TransformVerdict::Woken(3));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 2_300), ClaimVerdict::Moving);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, true, 10.0, 2_400)), TransformVerdict::Returned(4));
        assert_eq!(p.claim(BASKET, C2, CELL, touch::CONTACT, 2_500), ClaimVerdict::Granted(5));
        // nothing of c3's: nothing returned
        assert_eq!(p.gone(C3, 3_000), vec![]);
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
        // an asleep sample over the budget: the rest is taken (the prop the host's again, a word), the position not
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, true, 5_000.0, 1_400)), TransformVerdict::RestRefused(2));
        assert_eq!(p.owner_of(BASKET, C1, 1_400), C1);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C1, false, 10.0, 1_500)), TransformVerdict::NotOwner);
        // the host's first sample of an unclaimed prop: half a second of
        // flight (PROP_FIRST_SAMPLE_UNITS, playtest 41's cabbages), then the
        // row's elapsed budget
        let mut q = Props::default();
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, PROP_FIRST_SAMPLE_UNITS + 1.0, 0)), TransformVerdict::TooFast);
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, 200.0, 0)), TransformVerdict::Woken(1));
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, 200.0, 10)), TransformVerdict::TooFast);
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, 50.0, 10)), TransformVerdict::Accepted);
    }

    #[test]
    fn a_hosts_first_sample_of_a_blasted_prop_passes_and_writes_the_row() {
        // playtest 41's cabbages: the host's first sample after a shout's
        // blast moved 500 units with no row; refused by the floor, the prop
        // never synced again. The first sample has half a second of flight
        let mut p = Props::default();
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 500.0, 1_000)), TransformVerdict::Woken(1));
        // the row is written: the next sample has the elapsed budget (100 ms
        // at the top speed is 600 units) and the floor under it
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 590.0, 1_100)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C1, false, 700.0, 1_200)), TransformVerdict::TooFast);
        // a first sample beyond half a second of flight is still a teleport
        let mut q = Props::default();
        assert_eq!(q.transform(&facts(0x54a44, C1, CELL, C1, false, PROP_FIRST_SAMPLE_UNITS + 1.0, 1_000)), TransformVerdict::TooFast);
        // a blast on a prop the host streams awake: the blaster's impulse claim is refused, the host keeps it
        assert_eq!(p.claim(BASKET, C2, CELL, touch::IMPULSE, 1_150), ClaimVerdict::Moving);
        assert_eq!(p.owner_of(BASKET, C1, 1_150), C1);
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
        assert_eq!(p.live_owner(BASKET, 1_000), C1);
        p.host_changed(CELL, C2, 1_100);
        assert_eq!(p.owner_of(BASKET, C2, 1_200), C1);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C2, false, 10.0, 1_200)), TransformVerdict::NotOwner);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, false, 10.0, 1_200)), TransformVerdict::Accepted);
        assert_eq!(p.transform(&facts(BASKET, C1, CELL, C2, true, 10.0, 1_300)), TransformVerdict::Returned(2));
        assert_eq!(p.owner_of(BASKET, C2, 1_300), C2);
        assert_eq!(p.transform(&facts(BASKET, C2, CELL, C2, false, 10.0, 1_400)), TransformVerdict::Woken(3));
        // a non-host's sample never wakes a resting prop
        assert_eq!(p.transform(&facts(BASKET, C3, CELL, C2, false, 10.0, 1_500)), TransformVerdict::NotOwner);
        // the host's sample of a grab past its cap, before the tick returns it: a wake too, the grab mark gone
        let mut q = Props::default();
        assert_eq!(q.claim(BASKET, C2, CELL, touch::GRAB, 1_000), ClaimVerdict::Granted(1));
        assert_eq!(q.transform(&facts(BASKET, C1, CELL, C1, false, 10.0, 1_000 + PROP_GRAB_MAX_MS)), TransformVerdict::Woken(2));
        assert_eq!(q.live_owner(BASKET, 1_000 + PROP_GRAB_MAX_MS), C1);
        assert_eq!(q.tick(1_100 + PROP_GRAB_MAX_MS), vec![]);
    }
}
