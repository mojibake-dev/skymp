//! Cell hosts (thuum docs/verbs/cell-host.md, ADR-035, M3). One player's game
//! runs a cell's NPCs and the server knows which: the first game to have
//! the cell fully loaded while the server's interest grid holds it (whichever
//! of the two comes second: the game loads a 5x5 and the grid is a 3x3, so a
//! cell is often loaded before it is reached); kept while both stand; when
//! either lapses (the cell unloaded, the grid moved off it, or the player
//! gone) a grace window runs, disarmed when both stand again, and at its end
//! the remaining visitors with the cell loaded and in reach rank by round
//! trip, lowest first, earliest loaded on a tie; the
//! first gets the grant, the old host its release; none, and the cell is
//! unhosted, frozen at its record. Every grant carries an epoch, one counter
//! per cell, up at every grant and kept while the cell is unhosted, so a
//! report or a grant from an older epoch is dropped (a fencing token). Hosts
//! never churn: a second loader of a hosted cell gets nothing, and nobody
//! takes a cell by ping. Times are a monotonic clock's milliseconds; the core
//! gathers the facts (the loaded reports, the grid, the round trips, the
//! disconnects) and asks here.

use std::collections::{HashMap, HashSet};

/// How long a host keeps its cell after unloading it or leaving, before the
/// cell passes on (ADR-035, Eli 2026-10-18: "a grace window of about 5 s").
pub const GRACE_MS: u64 = 5_000;

/// How many CellLoaded reports a client may send at once; the validator's
/// budget, carried with its conditions (ADR-033). Measured on the lab's
/// first run of b-cell-host (20261021-184501, fork c0e4a029): the save's
/// load fired 30 loaded reports within 14 ms, a teleport five cells away 25
/// unloaded and 25 loaded within 60 ms (loads and unloads share the
/// budget), and the first bucket of 32 then 8 a second refused the landing
/// cell's own report (E_VAL_RATE), which no client retries. Two such
/// teleports inside five seconds are 100 reports; a report is one bit set,
/// so the bucket is sized for the engine's bursts, not for a byte budget.
pub const CELL_LOADED_BURST: u32 = 128;

/// How many CellLoaded reports a second after the burst.
pub const CELL_LOADED_PER_SECOND: u32 = 32;

/// What the server decides; the core sends each to its game or applies it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Decision {
    /// `player` now hosts `cell` at `epoch`.
    Grant {
        /// The player whose game hosts the cell now.
        player: u32,
        /// The cell.
        cell: u32,
        /// The grant's epoch, the cell's counter after it.
        epoch: u32,
    },
    /// `player` stops hosting `cell`; `epoch` is the grant it ends.
    Release {
        /// The player whose game stops hosting.
        player: u32,
        /// The cell.
        cell: u32,
        /// The epoch of the grant that ends.
        epoch: u32,
    },
    /// Nobody hosts `cell`: it freezes at its record, the epoch kept.
    Unhosted {
        /// The cell.
        cell: u32,
    },
}

#[derive(Debug, Clone, Copy)]
struct Host {
    player: u32,
    epoch: u32,
    /// Set when the host unloaded the cell or left; the grace runs from it.
    leaving_since_ms: Option<u64>,
}

#[derive(Debug, Clone, Copy, Default)]
struct CellState {
    host: Option<Host>,
    /// The last grant's epoch, kept while unhosted.
    epoch: u32,
}

#[derive(Debug, Clone, Default)]
struct PlayerState {
    /// The cells the game reports loaded, each with the time it first did.
    loaded: HashMap<u32, u64>,
    /// The cells the server's grid gives the player (its 3x3).
    reach: HashSet<u32>,
    /// The connection's round trip, when the transport has measured one.
    rtt_ms: Option<u32>,
    /// The connection has gone silent (no message for the server's stale
    /// threshold while netcode still holds it connected): no candidate
    /// anywhere, its hosted cells in their grace, its loaded set kept for
    /// its return (thuum docs/verbs/cell-host.md, the quitting host).
    stale: bool,
}

/// Every cell's host and every player's loaded set, grid and round trip.
#[derive(Debug, Default)]
pub struct Cells {
    cells: HashMap<u32, CellState>,
    players: HashMap<u32, PlayerState>,
}

impl Cells {
    /// The cells the server's grid gives `player` now (its 3x3 of 4096-unit
    /// chunks, one exterior grid cell each, or its interior) at `now_ms`:
    /// a cell newly reached that the game has loaded and nobody hosts is
    /// granted; a hosted cell newly reached disarms its grace; a hosted cell
    /// the grid dropped arms it.
    pub fn reach(&mut self, player: u32, cells: &[u32], now_ms: u64) -> Vec<Decision> {
        let next: HashSet<u32> = cells.iter().copied().collect();
        let prev = std::mem::replace(&mut self.players.entry(player).or_default().reach, next.clone());
        let mut touched: Vec<u32> = prev.symmetric_difference(&next).copied().collect();
        touched.sort_unstable();
        touched.into_iter().filter_map(|cell| self.consider(player, cell, now_ms)).collect()
    }

    /// The transport's round trip for `player`'s connection, milliseconds.
    pub fn rtt(&mut self, player: u32, ms: u32) {
        self.players.entry(player).or_default().rtt_ms = Some(ms);
    }

    /// `player` reports `cell` fully loaded at `now_ms`: a grant when its
    /// reach holds the cell and nobody hosts it; its own cell's grace is
    /// disarmed when the reach holds it; a cell another player hosts gives
    /// nothing.
    pub fn loaded(&mut self, player: u32, cell: u32, now_ms: u64) -> Vec<Decision> {
        self.players.entry(player).or_default().loaded.entry(cell).or_insert(now_ms);
        self.consider(player, cell, now_ms).into_iter().collect()
    }

    /// `player` reports `cell` unloaded at `now_ms`: its grace starts when it
    /// hosts the cell; otherwise it only stops being a candidate.
    pub fn unloaded(&mut self, player: u32, cell: u32, now_ms: u64) -> Vec<Decision> {
        if let Some(p) = self.players.get_mut(&player) {
            p.loaded.remove(&cell);
        }
        self.consider(player, cell, now_ms).into_iter().collect()
    }

    /// `player` left the server at `now_ms`: every cell it hosts starts its
    /// grace, and it is no candidate anywhere.
    pub fn gone(&mut self, player: u32, now_ms: u64) -> Vec<Decision> {
        self.players.remove(&player);
        for state in self.cells.values_mut() {
            if let Some(ref mut host) = state.host {
                if host.player == player && host.leaving_since_ms.is_none() {
                    host.leaving_since_ms = Some(now_ms);
                }
            }
        }
        Vec::new()
    }

    /// `player`'s connection went silent at `now_ms` (the game killed,
    /// crashed or quit while netcode's own timeout still runs): every cell
    /// it hosts starts its grace and it is no candidate anywhere, but its
    /// loaded set and reach stay, so a return within netcode's patience is
    /// `revived` and not a fresh arrival.
    pub fn stale(&mut self, player: u32, now_ms: u64) -> Vec<Decision> {
        self.players.entry(player).or_default().stale = true;
        for state in self.cells.values_mut() {
            if let Some(ref mut host) = state.host {
                if host.player == player && host.leaving_since_ms.is_none() {
                    host.leaving_since_ms = Some(now_ms);
                }
            }
        }
        Vec::new()
    }

    /// `player`'s messages resumed at `now_ms`: a candidate again; each cell
    /// it has loaded is considered anew, which disarms its own grace where
    /// it still hosts and grants a free cell the grid gives it.
    pub fn revived(&mut self, player: u32, now_ms: u64) -> Vec<Decision> {
        let Some(p) = self.players.get_mut(&player) else {
            return Vec::new();
        };
        p.stale = false;
        let mut cells: Vec<u32> = p.loaded.keys().copied().collect();
        cells.sort_unstable();
        cells.into_iter().filter_map(|cell| self.consider(player, cell, now_ms)).collect()
    }

    /// Whether `player` has `cell` loaded and in reach, and is heard from:
    /// what hosting takes.
    fn eligible(&self, player: u32, cell: u32) -> bool {
        self.players
            .get(&player)
            .is_some_and(|p| !p.stale && p.reach.contains(&cell) && p.loaded.contains_key(&cell))
    }

    /// `player`'s standing toward `cell` changed at `now_ms`: a grant when
    /// it is eligible and nobody hosts; its own grace disarmed while
    /// eligible, armed while not; nothing toward another's cell.
    fn consider(&mut self, player: u32, cell: u32, now_ms: u64) -> Option<Decision> {
        let eligible = self.eligible(player, cell);
        let state = self.cells.entry(cell).or_default();
        match state.host {
            Some(ref mut host) if host.player == player => {
                if eligible {
                    host.leaving_since_ms = None;
                } else if host.leaving_since_ms.is_none() {
                    host.leaving_since_ms = Some(now_ms);
                }
                None
            }
            Some(_) => None,
            None if eligible => {
                let epoch = state.epoch.saturating_add(1);
                state.epoch = epoch;
                state.host = Some(Host { player, epoch, leaving_since_ms: None });
                Some(Decision::Grant { player, cell, epoch })
            }
            None => None,
        }
    }

    /// The clock at `now_ms`: every grace that has run out passes its cell on,
    /// to the visitor with the cell loaded and in reach whose round trip is
    /// lowest (earliest loaded on a tie), or to nobody.
    pub fn tick(&mut self, now_ms: u64) -> Vec<Decision> {
        let mut due: Vec<(u32, Host)> = Vec::new();
        for (&cell, state) in &self.cells {
            if let Some(host) = state.host {
                if let Some(since) = host.leaving_since_ms {
                    if now_ms.saturating_sub(since) >= GRACE_MS {
                        due.push((cell, host));
                    }
                }
            }
        }
        due.sort_by_key(|&(cell, _)| cell);
        let mut out = Vec::new();
        for (cell, old) in due {
            out.push(Decision::Release { player: old.player, cell, epoch: old.epoch });
            let next = self.candidate(cell, old.player);
            let Some(state) = self.cells.get_mut(&cell) else { continue };
            match next {
                Some(player) => {
                    let epoch = state.epoch.saturating_add(1);
                    state.epoch = epoch;
                    state.host = Some(Host { player, epoch, leaving_since_ms: None });
                    out.push(Decision::Grant { player, cell, epoch });
                }
                None => {
                    state.host = None;
                    out.push(Decision::Unhosted { cell });
                }
            }
        }
        out
    }

    /// The visitor to take `cell` from `leaving`: the cell loaded and in
    /// reach, the lowest round trip first (a connection without one ranks
    /// last), the earliest loaded on a tie, the lower id on a full tie.
    fn candidate(&self, cell: u32, leaving: u32) -> Option<u32> {
        self.players
            .iter()
            .filter(|&(&id, p)| id != leaving && !p.stale && p.reach.contains(&cell) && p.loaded.contains_key(&cell))
            .map(|(&id, p)| (p.rtt_ms.unwrap_or(u32::MAX), p.loaded.get(&cell).copied().unwrap_or(u64::MAX), id))
            .min()
            .map(|(_, _, id)| id)
    }

    /// Who hosts `cell`, with the grant's epoch; nobody while unhosted.
    #[must_use]
    pub fn host_of(&self, cell: u32) -> Option<(u32, u32)> {
        self.cells.get(&cell).and_then(|s| s.host.map(|h| (h.player, h.epoch)))
    }

    /// Whether a report from `player` for an actor in `cell` at `epoch` is
    /// the host's at the current grant.
    #[must_use]
    pub fn accepts(&self, cell: u32, player: u32, epoch: u32) -> bool {
        self.host_of(cell) == Some((player, epoch))
    }

    /// The cell's epoch, kept while unhosted; 0 before any grant.
    #[must_use]
    pub fn epoch_of(&self, cell: u32) -> u32 {
        self.cells.get(&cell).map_or(0, |s| s.epoch)
    }

    /// Every cell the table knows, by cell id: its host (`None` while
    /// unhosted) and its epoch, for the lab's reading of the server.
    #[must_use]
    pub fn hosts(&self) -> Vec<(u32, Option<u32>, u32)> {
        let mut rows: Vec<(u32, Option<u32>, u32)> =
            self.cells.iter().map(|(&cell, s)| (cell, s.host.map(|h| h.player), s.epoch)).collect();
        rows.sort_unstable_by_key(|r| r.0);
        rows
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const RIVERWOOD: u32 = 0x9732;
    const C1: u32 = 0xff00_0000;
    const C2: u32 = 0xff00_0001;

    fn both_in_reach() -> Cells {
        let mut c = Cells::default();
        c.reach(C1, &[RIVERWOOD], 0);
        c.reach(C2, &[RIVERWOOD], 0);
        c
    }

    #[test]
    fn the_first_loader_hosts_and_a_second_gets_nothing() {
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C2, RIVERWOOD, 1_000), vec![]);
        assert_eq!(c.host_of(RIVERWOOD), Some((C1, 1)));
        assert!(c.accepts(RIVERWOOD, C1, 1));
        assert!(!c.accepts(RIVERWOOD, C2, 1));
        assert!(!c.accepts(RIVERWOOD, C1, 2));
        // a load the grid does not give: no grant
        let mut d = Cells::default();
        d.reach(C1, &[0x1234], 0);
        assert_eq!(d.loaded(C1, RIVERWOOD, 1_000), vec![]);
        assert_eq!(d.host_of(RIVERWOOD), None);
    }

    #[test]
    fn the_grace_is_disarmed_by_a_return_and_fires_at_its_end() {
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        c.loaded(C2, RIVERWOOD, 1_500);
        assert_eq!(c.unloaded(C1, RIVERWOOD, 10_000), vec![]);
        assert_eq!(c.tick(10_000 + GRACE_MS - 100), vec![]);
        // back inside the grace: still the host, same epoch
        assert_eq!(c.loaded(C1, RIVERWOOD, 10_000 + GRACE_MS - 100), vec![]);
        assert_eq!(c.tick(10_000 + GRACE_MS + 1_000), vec![]);
        assert_eq!(c.host_of(RIVERWOOD), Some((C1, 1)));
        // away past the grace: the visitor takes it at the next epoch
        c.unloaded(C1, RIVERWOOD, 20_000);
        assert_eq!(c.tick(20_000 + GRACE_MS - 1), vec![]);
        assert_eq!(
            c.tick(20_000 + GRACE_MS),
            vec![
                Decision::Release { player: C1, cell: RIVERWOOD, epoch: 1 },
                Decision::Grant { player: C2, cell: RIVERWOOD, epoch: 2 },
            ]
        );
        assert_eq!(c.host_of(RIVERWOOD), Some((C2, 2)));
        assert!(!c.accepts(RIVERWOOD, C1, 1));
    }

    #[test]
    fn the_lowest_round_trip_wins_and_the_earliest_on_a_tie() {
        let c3: u32 = 0xff00_0002;
        let mut c = both_in_reach();
        c.reach(c3, &[RIVERWOOD], 0);
        c.loaded(C1, RIVERWOOD, 1_000);
        c.loaded(C2, RIVERWOOD, 2_000);
        c.loaded(c3, RIVERWOOD, 3_000);
        c.rtt(C2, 40);
        c.rtt(c3, 20);
        c.gone(C1, 5_000);
        let out = c.tick(5_000 + GRACE_MS);
        assert!(out.contains(&Decision::Grant { player: c3, cell: RIVERWOOD, epoch: 2 }));
        // a tie on the round trip: the earliest loaded
        let mut t = both_in_reach();
        t.reach(c3, &[RIVERWOOD], 0);
        t.loaded(C1, RIVERWOOD, 1_000);
        t.loaded(c3, RIVERWOOD, 2_000);
        t.loaded(C2, RIVERWOOD, 3_000);
        t.rtt(C2, 30);
        t.rtt(c3, 30);
        t.unloaded(C1, RIVERWOOD, 4_000);
        let out = t.tick(4_000 + GRACE_MS);
        assert!(out.contains(&Decision::Grant { player: c3, cell: RIVERWOOD, epoch: 2 }));
        // a connection without a round trip ranks last
        let mut u = both_in_reach();
        u.loaded(C1, RIVERWOOD, 1_000);
        u.loaded(C2, RIVERWOOD, 1_000);
        u.reach(c3, &[RIVERWOOD], 0);
        u.loaded(c3, RIVERWOOD, 500);
        u.rtt(C2, 300);
        u.unloaded(C1, RIVERWOOD, 2_000);
        let out = u.tick(2_000 + GRACE_MS);
        assert!(out.contains(&Decision::Grant { player: C2, cell: RIVERWOOD, epoch: 2 }));
    }

    #[test]
    fn no_visitor_leaves_the_cell_unhosted_with_its_epoch_kept() {
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        c.gone(C1, 2_000);
        assert_eq!(
            c.tick(2_000 + GRACE_MS),
            vec![Decision::Release { player: C1, cell: RIVERWOOD, epoch: 1 }, Decision::Unhosted { cell: RIVERWOOD }]
        );
        assert_eq!(c.host_of(RIVERWOOD), None);
        assert_eq!(c.epoch_of(RIVERWOOD), 1);
        assert!(!c.accepts(RIVERWOOD, C1, 1));
        assert_eq!(c.hosts(), vec![(RIVERWOOD, None, 1)]);
        // the next loader takes the next epoch
        assert_eq!(c.loaded(C2, RIVERWOOD, 9_000), vec![Decision::Grant { player: C2, cell: RIVERWOOD, epoch: 2 }]);
        assert_eq!(c.hosts(), vec![(RIVERWOOD, Some(C2), 2)]);
        // a visitor who unloaded before the deadline is no candidate
        let mut d = both_in_reach();
        d.loaded(C1, RIVERWOOD, 1_000);
        d.loaded(C2, RIVERWOOD, 1_000);
        d.unloaded(C1, RIVERWOOD, 2_000);
        d.unloaded(C2, RIVERWOOD, 3_000);
        assert_eq!(
            d.tick(2_000 + GRACE_MS),
            vec![Decision::Release { player: C1, cell: RIVERWOOD, epoch: 1 }, Decision::Unhosted { cell: RIVERWOOD }]
        );
    }

    #[test]
    fn a_player_gone_is_an_unload_everywhere_and_a_restart_counts_from_one() {
        let inn: u32 = 0x1_3480;
        let mut c = both_in_reach();
        c.reach(C1, &[RIVERWOOD, inn], 0);
        c.loaded(C1, RIVERWOOD, 1_000);
        c.loaded(C1, inn, 1_000);
        c.loaded(C2, RIVERWOOD, 1_500);
        c.gone(C1, 2_000);
        let out = c.tick(2_000 + GRACE_MS);
        assert_eq!(out.len(), 4);
        assert!(out.contains(&Decision::Grant { player: C2, cell: RIVERWOOD, epoch: 2 }));
        assert!(out.contains(&Decision::Unhosted { cell: inn }));
        // a gone player's late report is not a candidate's
        assert_eq!(c.loaded(C1, inn, 9_000), vec![]);
        // a restart: a fresh table, the first grant at epoch 1
        let mut fresh = both_in_reach();
        assert_eq!(fresh.loaded(C2, RIVERWOOD, 100), vec![Decision::Grant { player: C2, cell: RIVERWOOD, epoch: 1 }]);
    }

    #[test]
    fn a_cell_loaded_before_it_is_reached_is_granted_when_the_grid_arrives_and_the_grid_leaving_arms_the_grace() {
        let far: u32 = 0x9733;
        let mut c = both_in_reach();
        // the game's 5x5 loads the next cell while the grid's 3x3 does not reach it
        assert_eq!(c.loaded(C1, far, 1_000), vec![]);
        assert_eq!(c.host_of(far), None);
        // the player walks on: the grid reaches it, the cell is granted
        assert_eq!(c.reach(C1, &[RIVERWOOD, far], 2_000), vec![Decision::Grant { player: C1, cell: far, epoch: 1 }]);
        // a second player reaching a hosted cell gets nothing
        c.loaded(C2, far, 2_500);
        assert_eq!(c.reach(C2, &[RIVERWOOD, far], 3_000), vec![]);
        // the host's grid drops the cell (still loaded in its 5x5): the grace arms
        assert_eq!(c.reach(C1, &[RIVERWOOD], 4_000), vec![]);
        assert_eq!(c.tick(4_000 + GRACE_MS - 1), vec![]);
        // back inside the grace: disarmed, same epoch
        assert_eq!(c.reach(C1, &[RIVERWOOD, far], 4_000 + GRACE_MS - 1), vec![]);
        assert_eq!(c.tick(4_000 + GRACE_MS + 1_000), vec![]);
        assert_eq!(c.host_of(far), Some((C1, 1)));
        // off again past the grace: the visitor who has it loaded and in reach takes it
        c.reach(C1, &[RIVERWOOD], 20_000);
        assert_eq!(
            c.tick(20_000 + GRACE_MS),
            vec![Decision::Release { player: C1, cell: far, epoch: 1 }, Decision::Grant { player: C2, cell: far, epoch: 2 }]
        );
        // a load report while out of reach disarms nothing
        let mut d = both_in_reach();
        d.loaded(C1, RIVERWOOD, 1_000);
        d.reach(C1, &[], 2_000);
        assert_eq!(d.loaded(C1, RIVERWOOD, 3_000), vec![]);
        assert_eq!(
            d.tick(2_000 + GRACE_MS),
            vec![Decision::Release { player: C1, cell: RIVERWOOD, epoch: 1 }, Decision::Unhosted { cell: RIVERWOOD }]
        );
    }

    #[test]
    fn a_same_tick_double_load_grants_once() {
        let mut c = both_in_reach();
        let a = c.loaded(C1, RIVERWOOD, 1_000);
        let b = c.loaded(C2, RIVERWOOD, 1_000);
        assert_eq!(a.len(), 1);
        assert!(b.is_empty());
    }

    #[test]
    fn a_silent_host_loses_its_cell_at_the_grace_and_a_revived_one_is_a_candidate_again() {
        // thuum docs/verbs/cell-host.md, the quitting host (playtest 33):
        // the village belonged to a gone game for netcode's timeout plus
        // the grace, about 24 s; the server now reads the silence itself
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C2, RIVERWOOD, 1_500), vec![]);
        // silence: the grace starts, nothing passes before it runs out
        assert_eq!(c.stale(C1, 10_000), vec![]);
        assert_eq!(c.tick(10_000 + GRACE_MS - 100), vec![]);
        assert_eq!(c.host_of(RIVERWOOD), Some((C1, 1)));
        // the grace runs out: the visitor with the cell loaded takes it
        assert_eq!(
            c.tick(10_000 + GRACE_MS),
            vec![
                Decision::Release { player: C1, cell: RIVERWOOD, epoch: 1 },
                Decision::Grant { player: C2, cell: RIVERWOOD, epoch: 2 }
            ]
        );
        // the old host's messages resume: a candidate again, nothing to take
        assert_eq!(c.revived(C1, 20_000), vec![]);
        assert_eq!(c.host_of(RIVERWOOD), Some((C2, 2)));
        // the new host leaves: the revived one, still loaded and in reach, is next
        assert_eq!(c.unloaded(C2, RIVERWOOD, 30_000), vec![]);
        assert_eq!(
            c.tick(30_000 + GRACE_MS),
            vec![
                Decision::Release { player: C2, cell: RIVERWOOD, epoch: 2 },
                Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 3 }
            ]
        );
    }

    #[test]
    fn a_host_heard_again_inside_the_grace_keeps_its_cell_and_a_stale_player_is_no_candidate() {
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        c.loaded(C2, RIVERWOOD, 1_500);
        assert_eq!(c.stale(C1, 10_000), vec![]);
        // heard again before the grace runs out: disarmed, same epoch
        assert_eq!(c.revived(C1, 10_000 + 1_000), vec![]);
        assert_eq!(c.tick(10_000 + GRACE_MS + 1_000), vec![]);
        assert_eq!(c.host_of(RIVERWOOD), Some((C1, 1)));
        // a stale visitor never takes a cell: the host leaves, the cell is nobody's
        assert_eq!(c.stale(C2, 20_000), vec![]);
        assert_eq!(c.unloaded(C1, RIVERWOOD, 20_000), vec![]);
        assert_eq!(
            c.tick(20_000 + GRACE_MS),
            vec![Decision::Release { player: C1, cell: RIVERWOOD, epoch: 1 }, Decision::Unhosted { cell: RIVERWOOD }]
        );
        // and takes it the moment it is heard again
        assert_eq!(c.revived(C2, 30_000), vec![Decision::Grant { player: C2, cell: RIVERWOOD, epoch: 2 }]);
    }
}
