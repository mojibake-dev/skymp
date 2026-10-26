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

use std::collections::HashMap;

/// How long a host keeps its cell after unloading it or leaving, before the
/// cell passes on (ADR-035, Eli 2026-10-18: "a grace window of about 5 s").
pub const GRACE_MS: u64 = 5_000;

/// How long after a cold start (no cell ever hosted) the first grants stay
/// provisional: a nearer eligible player arriving inside it takes the cell
/// at once, without a grace, the one time a host changes without one
/// (ADR-047 D10: a restart's two reconnects came 20 ms apart and split
/// Riverwood nine cells to three by arrival order). UNMEASURED placeholder;
/// b-npc-handoff's restart step sizes it.
pub const COLD_START_MS: u64 = 5_000;

/// How long after a follow through a door a report putting the actor back
/// in the cell it left is refused as stale (x-handoff-door 20261024-013234:
/// the host's report 133 ms after the follow said the exterior again and the
/// record bounced out; a sample taken before the engine's transit, or
/// reordered on the unreliable channel). A real return after the window
/// passes as before. UNMEASURED placeholder.
pub const FOLLOW_SETTLE_MS: u64 = 1_000;

/// The pass's hysteresis (ADR-047 D22, its brake; thuum docs/verbs/npc-handoff.md,
/// 2026-10-26): a simulator's crossing into another live host's cell passes the
/// actor only once the report stands this many units inside the destination's
/// square; nearer the edge the crossing host keeps it (a follow into the hosted
/// cell), so an actor working the border (Alvor between 9731 and 9732: 836
/// passes in one green run of b-npc-handoff-cross, 2026-10-26) changes hands at
/// most once per real crossing. UNMEASURED: the lab's read sets it.
pub const PASS_MARGIN_UNITS: u32 = 256;

/// What the server answers a simulator's report that moves an actor
/// (ADR-047 D6, thuum docs/verbs/npc-handoff.md): the core turns it into
/// the accept, the follow's record or the drop.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum MoveVerdict {
    /// Dropped: the sender does not simulate the actor where it stands, or
    /// the destination is no cell, or another game hosts it and the move is
    /// no adjacent crossing by the actor's simulator.
    Refuse = 0,
    /// Applied as today: the move stays inside the sender's cell, or enters
    /// a cell the sender hosts (which ends any follow of the actor).
    Accept = 1,
    /// The destination is nobody's and the sender vouches for it: the sender
    /// follows its actor there and simulates it until the cell is granted,
    /// the sender unloads it or leaves, or the actor enters a hosted cell.
    Follow = 2,
    /// The sender simulates the actor and walks it into an adjacent cell
    /// another live player hosts (ADR-047 D9, npc-handoff (d)): the record
    /// moves there and the simulation passes to that host at once, the word
    /// flipping both ways; the sender's further reports are dropped. Playtest
    /// 34 (2026-10-25): a restart's cold start split Sigrid's two cells
    /// between the players and every crossing was refused, seven a second,
    /// one engine walking her live where the other held the record.
    Pass = 3,
}

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
    /// When the standing host was granted (the cold start's provisional window).
    granted_ms: u64,
}

/// A simulator's report that moves an actor between cells, the facts the
/// core gathers for `Cells::may_move` (ADR-047 D6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CellMove {
    /// The reporting player's actor.
    pub sender: u32,
    /// The actor moved.
    pub actor: u32,
    /// The cell the actor's record stands in (0 for none).
    pub origin: u32,
    /// The cell the report puts it in (0 for none).
    pub dest: u32,
    /// Whether the two exterior grid squares touch in one worldspace.
    pub adjacent: bool,
    /// Whether the destination is an interior entered by a persistent actor.
    pub door: bool,
    /// How far the reported position stands inside the destination's exterior
    /// square, in units from its nearest edge (u32::MAX for an interior or an
    /// unknown): the pass's hysteresis reads it (PASS_MARGIN_UNITS).
    pub edge_units: u32,
}

/// A host simulating an actor in a cell nobody hosts (ADR-047 D6), or, within
/// the pass margin, in another host's cell it has just crossed into (D22).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Follow {
    player: u32,
    cell: u32,
    since_ms: u64,
}

#[derive(Debug, Clone, Default)]
struct PlayerState {
    /// The cells the game reports loaded, each with the time it first did.
    loaded: HashMap<u32, u64>,
    /// The cells the server's grid gives the player (its 3x3), each with
    /// its ring: 0 the player's own square, 1 a neighbour, the nearness the
    /// cold start ranks by (ADR-047 D10).
    reach: HashMap<u32, u8>,
    /// The connection's round trip, when the transport has measured one.
    rtt_ms: Option<u32>,
    /// The connection has gone silent (no message for the server's stale
    /// threshold while netcode still holds it connected): no candidate
    /// anywhere, its hosted cells in their grace, its loaded set kept for
    /// its return (thuum docs/verbs/cell-host.md, the quitting host).
    stale: bool,
}

/// Every cell's host and every player's loaded set, grid and round trip,
/// and the follows (ADR-047 D6).
#[derive(Debug, Default)]
pub struct Cells {
    cells: HashMap<u32, CellState>,
    players: HashMap<u32, PlayerState>,
    follows: HashMap<u32, Follow>,
    /// The first grant's time, the cold start's window opening; None until it.
    first_grant_ms: Option<u64>,
}

impl Cells {
    /// The cells the server's grid gives `player` now (its 3x3 of 4096-unit
    /// chunks, one exterior grid cell each, or its interior) at `now_ms`:
    /// a cell newly reached that the game has loaded and nobody hosts is
    /// granted; a hosted cell newly reached disarms its grace; a hosted cell
    /// the grid dropped arms it.
    pub fn reach(&mut self, player: u32, cells: &[u32], now_ms: u64) -> Vec<Decision> {
        let rings = vec![1u8; cells.len()];
        self.reach_ranked(player, cells, &rings, now_ms)
    }

    /// `reach` with each cell's ring (0 the player's own square, 1 a
    /// neighbour; a missing ring counts as 1), the nearness the cold start
    /// ranks by (ADR-047 D10).
    pub fn reach_ranked(&mut self, player: u32, cells: &[u32], rings: &[u8], now_ms: u64) -> Vec<Decision> {
        let next: HashMap<u32, u8> =
            cells.iter().enumerate().map(|(i, &c)| (c, rings.get(i).copied().unwrap_or(1))).collect();
        let prev = std::mem::replace(&mut self.players.entry(player).or_default().reach, next.clone());
        let mut touched: Vec<u32> = prev
            .keys()
            .filter(|c| !next.contains_key(c))
            .chain(next.keys().filter(|c| !prev.contains_key(c)))
            .copied()
            .collect();
        // a cell newly reached, or reached from a nearer square, is considered (the cold start)
        touched.extend(next.iter().filter(|(c, r)| prev.get(c).is_some_and(|p| p != *r)).map(|(c, _)| *c));
        touched.sort_unstable();
        touched.dedup();
        touched.into_iter().flat_map(|cell| self.consider(player, cell, now_ms)).collect()
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
        self.consider(player, cell, now_ms)
    }

    /// `player` reports `cell` unloaded at `now_ms`: its grace starts when it
    /// hosts the cell; otherwise it only stops being a candidate.
    pub fn unloaded(&mut self, player: u32, cell: u32, now_ms: u64) -> Vec<Decision> {
        if let Some(p) = self.players.get_mut(&player) {
            p.loaded.remove(&cell);
        }
        // a follow into an exterior the follower no longer loads ends (ADR-047 D6)
        self.follows.retain(|_, f| !(f.player == player && f.cell == cell));
        self.consider(player, cell, now_ms)
    }

    /// `player` left the server at `now_ms`: every cell it hosts starts its
    /// grace, and it is no candidate anywhere.
    pub fn gone(&mut self, player: u32, now_ms: u64) -> Vec<Decision> {
        self.players.remove(&player);
        self.follows.retain(|_, f| f.player != player);
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
        cells.into_iter().flat_map(|cell| self.consider(player, cell, now_ms)).collect()
    }

    /// Whether `player` has `cell` loaded and in reach, and is heard from:
    /// what hosting takes.
    fn eligible(&self, player: u32, cell: u32) -> bool {
        self.players
            .get(&player)
            .is_some_and(|p| !p.stale && p.reach.contains_key(&cell) && p.loaded.contains_key(&cell))
    }

    /// `player`'s ring for `cell` (0 its own square, 1 a neighbour), 2 when
    /// the grid does not give it the cell.
    fn ring(&self, player: u32, cell: u32) -> u8 {
        self.players.get(&player).and_then(|p| p.reach.get(&cell).copied()).unwrap_or(2)
    }

    /// Whether the table is inside its cold start: nothing granted yet, or
    /// the first grant less than COLD_START_MS ago (ADR-047 D10).
    fn cold_start(&self, now_ms: u64) -> bool {
        self.first_grant_ms.is_none_or(|t| now_ms.saturating_sub(t) < COLD_START_MS)
    }

    /// `player` hosts `cell` from `now_ms` at the next epoch; any follow into
    /// the cell ends with the grant.
    fn grant(&mut self, player: u32, cell: u32, now_ms: u64) -> Decision {
        self.follows.retain(|_, f| f.cell != cell);
        self.first_grant_ms.get_or_insert(now_ms);
        let state = self.cells.entry(cell).or_default();
        let epoch = state.epoch.saturating_add(1);
        state.epoch = epoch;
        state.host = Some(Host { player, epoch, leaving_since_ms: None });
        state.granted_ms = now_ms;
        Decision::Grant { player, cell, epoch }
    }

    /// Who simulates `actor` standing in `cell`: the cell's host, else the
    /// follower whose follow names the cell, else nobody (0). The one answer
    /// the core's word, routing and sighting gate read (ADR-047 D6).
    #[must_use]
    pub fn simulator_of(&self, actor: u32, cell: u32) -> u32 {
        // a follow names the simulator first: into a cell nobody hosts (D6) or,
        // within the pass margin, into another host's cell the follower has
        // just crossed into (D22); a grant into the cell ends either
        if let Some(f) = self.follows.get(&actor) {
            if f.cell == cell {
                return f.player;
            }
        }
        match self.host_of(cell) {
            Some((player, _)) => player,
            None => 0,
        }
    }

    /// The verdict for the report `m` without recording anything (ADR-047
    /// D6): inside the sender's own cell, or into a cell it hosts, Accept;
    /// into a cell nobody hosts that the sender vouches for (adjacent and
    /// loaded by its game, or a door), Follow; the simulator's adjacent
    /// crossing into another live host's cell, Pass (D9); anything else
    /// Refuse. The
    /// door's pre-check reads this (x-handoff-door 20261024-001915: a verdict
    /// that wrote follow rows at the door removed a standing follow before
    /// the host was looked up, and recorded one before the host had crossed).
    #[must_use]
    pub fn judge(&self, m: &CellMove, now_ms: u64) -> MoveVerdict {
        let CellMove { sender, actor, origin, dest, adjacent, door, edge_units } = *m;
        // a report putting the actor back where it stood before a follow
        // younger than the settle window is a stale sample, not a return
        if dest != origin && dest != 0 && origin != 0 {
            if let Some(f) = self.follows.get(&actor) {
                if f.cell == origin && f.player == sender && now_ms.saturating_sub(f.since_ms) < FOLLOW_SETTLE_MS {
                    return MoveVerdict::Refuse;
                }
            }
        }
        if dest == 0 {
            return MoveVerdict::Refuse;
        }
        let simulator = if origin == 0 { 0 } else { self.simulator_of(actor, origin) };
        if dest == origin {
            if simulator != sender {
                return MoveVerdict::Refuse;
            }
            // a follow within the pass margin into another host's cell ends
            // once the actor stands the margin deep: the pass (D22)
            let margin_follow = self
                .follows
                .get(&actor)
                .is_some_and(|f| f.cell == dest && f.player == sender)
                && self.host_of(dest).is_some_and(|(host, _)| host != sender);
            return if margin_follow && edge_units >= PASS_MARGIN_UNITS { MoveVerdict::Pass } else { MoveVerdict::Accept };
        }
        match self.host_of(dest) {
            Some((host, _)) if host == sender => {
                if simulator == 0 || simulator == sender {
                    MoveVerdict::Accept
                } else {
                    MoveVerdict::Refuse
                }
            }
            Some(_) => {
                // another live host's cell: the simulator's own adjacent
                // crossing passes the actor to that host (D9) once it stands
                // the margin inside (D22's brake); nearer the edge the
                // crossing host keeps it, a follow into the hosted cell, so a
                // border worked back and forth changes hands once at most; a
                // teleport across squares or a stranger's report is dropped
                if simulator == sender && adjacent {
                    if edge_units >= PASS_MARGIN_UNITS {
                        MoveVerdict::Pass
                    } else {
                        MoveVerdict::Follow
                    }
                } else {
                    MoveVerdict::Refuse
                }
            }
            None => {
                if simulator != sender {
                    return MoveVerdict::Refuse;
                }
                let loads = self.players.get(&sender).is_some_and(|p| p.loaded.contains_key(&dest));
                if (adjacent && loads) || door {
                    MoveVerdict::Follow
                } else {
                    MoveVerdict::Refuse
                }
            }
        }
    }

    /// The report `m` at `now_ms`: `judge`'s verdict, applied. A Follow is
    /// recorded (actor to the sender, the destination, now); an Accept into a
    /// cell the sender hosts ends any follow of the actor.
    pub fn may_move(&mut self, m: &CellMove, now_ms: u64) -> MoveVerdict {
        let verdict = self.judge(m, now_ms);
        match verdict {
            MoveVerdict::Follow => {
                self.follows.insert(m.actor, Follow { player: m.sender, cell: m.dest, since_ms: now_ms });
            }
            MoveVerdict::Accept if m.dest != m.origin => {
                self.follows.remove(&m.actor);
            }
            MoveVerdict::Pass => {
                // the destination's host simulates it from here: any follow ends
                self.follows.remove(&m.actor);
            }
            _ => {}
        }
        verdict
    }

    /// Every follow, by actor id: the actor, its follower and the cell, for
    /// the lab's reading of the server.
    #[must_use]
    pub fn follows(&self) -> Vec<(u32, u32, u32)> {
        let mut rows: Vec<(u32, u32, u32)> = self.follows.iter().map(|(&a, f)| (a, f.player, f.cell)).collect();
        rows.sort_unstable();
        rows
    }

    /// `player`'s standing toward `cell` changed at `now_ms`: a grant when
    /// it is eligible and nobody hosts; its own grace disarmed while
    /// eligible, armed while not; nothing toward another's cell, except
    /// inside the cold start, where a provisional grant passes at once to an
    /// eligible player standing nearer the cell (ADR-047 D10).
    fn consider(&mut self, player: u32, cell: u32, now_ms: u64) -> Vec<Decision> {
        let eligible = self.eligible(player, cell);
        let cold = self.cold_start(now_ms);
        let state = self.cells.entry(cell).or_default();
        match state.host {
            Some(ref mut host) if host.player == player => {
                if eligible {
                    host.leaving_since_ms = None;
                } else if host.leaving_since_ms.is_none() {
                    host.leaving_since_ms = Some(now_ms);
                }
                Vec::new()
            }
            Some(host) => {
                let provisional = cold && now_ms.saturating_sub(state.granted_ms) < COLD_START_MS;
                if eligible && provisional && self.ring(player, cell) < self.ring(host.player, cell) {
                    let release = Decision::Release { player: host.player, cell, epoch: host.epoch };
                    let grant = self.grant(player, cell, now_ms);
                    vec![release, grant]
                } else {
                    Vec::new()
                }
            }
            None if eligible => vec![self.grant(player, cell, now_ms)],
            None => Vec::new(),
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
            match next {
                Some(player) => out.push(self.grant(player, cell, now_ms)),
                None => {
                    if let Some(state) = self.cells.get_mut(&cell) {
                        state.host = None;
                    }
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
            .filter(|&(&id, p)| id != leaving && !p.stale && p.reach.contains_key(&cell) && p.loaded.contains_key(&cell))
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

    #[test]
    fn a_host_follows_its_actor_into_a_cell_nobody_hosts_and_a_grant_ends_the_follow() {
        // ADR-047 D6: Stump into Gerdur's house was refused 751 times; the
        // host's own move into a cell it loads and nobody hosts is a follow
        const EAST: u32 = 0x9733;
        const STUMP: u32 = 0x1e62b;
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        // east is loaded by c1's game but outside everybody's grid
        assert_eq!(c.loaded(C1, EAST, 1_100), vec![]);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: STUMP, origin: RIVERWOOD, dest: EAST, adjacent: true, door: false, edge_units: u32::MAX }, 10_000), MoveVerdict::Follow);
        assert_eq!(c.simulator_of(STUMP, EAST), C1);
        assert_eq!(c.follows(), vec![(STUMP, C1, EAST)]);
        // inside the followed cell: the follower's moves pass, anyone else's are dropped
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: STUMP, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: u32::MAX }, 10_200), MoveVerdict::Accept);
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: STUMP, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: u32::MAX }, 10_300), MoveVerdict::Refuse);
        // a visitor who has not loaded east cannot follow anything there
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: 0x1348a, origin: RIVERWOOD, dest: EAST, adjacent: true, door: false, edge_units: u32::MAX }, 10_400), MoveVerdict::Refuse);
        // two squares away, or not loaded: refused
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: 0x1348a, origin: RIVERWOOD, dest: 0x9734, adjacent: false, door: false, edge_units: u32::MAX }, 10_500), MoveVerdict::Refuse);
        // back into the sender's hosted cell: accepted, the follow ends
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: STUMP, origin: EAST, dest: RIVERWOOD, adjacent: true, door: false, edge_units: u32::MAX }, 11_000), MoveVerdict::Accept);
        assert_eq!(c.follows(), vec![]);
        assert_eq!(c.simulator_of(STUMP, EAST), 0);
        // out again, then c2 reaches and loads east: granted, and the follow ends with the grant
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: STUMP, origin: RIVERWOOD, dest: EAST, adjacent: true, door: false, edge_units: u32::MAX }, 12_000), MoveVerdict::Follow);
        assert_eq!(c.loaded(C2, EAST, 12_500), vec![]);
        assert_eq!(c.reach(C2, &[RIVERWOOD, EAST], 12_600), vec![Decision::Grant { player: C2, cell: EAST, epoch: 1 }]);
        assert_eq!(c.follows(), vec![]);
        assert_eq!(c.simulator_of(STUMP, EAST), C2);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: STUMP, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: u32::MAX }, 12_700), MoveVerdict::Refuse);
        // into another live host's cell: the simulator's adjacent crossing passes it (D9)
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: 0x1348a, origin: RIVERWOOD, dest: EAST, adjacent: true, door: false, edge_units: u32::MAX }, 12_800), MoveVerdict::Pass);
    }

    #[test]
    fn a_crossing_into_another_live_hosts_cell_passes_the_actor_to_that_host() {
        // ADR-047 D9, npc-handoff (d); playtest 34 (2026-10-25): a restart's
        // cold start gave 9712 to one player and 9731 to the other, and
        // Sigrid's shuttle between them was refused at every crossing
        const EAST: u32 = 0x9733;
        const SIGRID: u32 = 0x13483;
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C2, EAST, 1_100), vec![]);
        assert_eq!(c.reach(C2, &[RIVERWOOD, EAST], 1_200), vec![Decision::Grant { player: C2, cell: EAST, epoch: 1 }]);
        assert_eq!(c.simulator_of(SIGRID, RIVERWOOD), C1);
        // c1 walks her across the border into c2's cell: a pass, no follow left
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: SIGRID, origin: RIVERWOOD, dest: EAST, adjacent: true, door: false, edge_units: u32::MAX }, 2_000), MoveVerdict::Pass);
        assert_eq!(c.follows(), vec![]);
        assert_eq!(c.simulator_of(SIGRID, EAST), C2);
        // from there c1's reports are dropped and c2's pass
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: SIGRID, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: u32::MAX }, 2_100), MoveVerdict::Refuse);
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: SIGRID, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: u32::MAX }, 2_200), MoveVerdict::Accept);
        // and back across, c2 passes her to c1 the same way
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: SIGRID, origin: EAST, dest: RIVERWOOD, adjacent: true, door: false, edge_units: u32::MAX }, 3_000), MoveVerdict::Pass);
        assert_eq!(c.simulator_of(SIGRID, RIVERWOOD), C1);
        // a crossing that is no adjacent walk (a teleport across squares) and a stranger's report stay refused
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: SIGRID, origin: RIVERWOOD, dest: EAST, adjacent: false, door: false, edge_units: u32::MAX }, 3_100), MoveVerdict::Refuse);
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: SIGRID, origin: RIVERWOOD, dest: EAST, adjacent: true, door: false, edge_units: u32::MAX }, 3_200), MoveVerdict::Refuse);
    }

    #[test]
    fn a_door_is_a_follow_without_a_loaded_report_and_the_follow_ends_with_an_unload_or_a_departure() {
        // an interior is never in a loaded set unless the player is inside;
        // a persistent actor through a door is vouched for by the door
        const INN: u32 = 0x133c6;
        const EMBRY: u32 = 0x3550c;
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: EMBRY, origin: RIVERWOOD, dest: INN, adjacent: false, door: false, edge_units: u32::MAX }, 2_000), MoveVerdict::Refuse);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: EMBRY, origin: RIVERWOOD, dest: INN, adjacent: false, door: true, edge_units: u32::MAX }, 2_000), MoveVerdict::Follow);
        assert_eq!(c.simulator_of(EMBRY, INN), C1);
        // the follower leaves: the follow goes with it
        assert_eq!(c.gone(C1, 3_000), vec![]);
        assert_eq!(c.simulator_of(EMBRY, INN), 0);
        assert_eq!(c.follows(), vec![]);
        // an exterior follow ends when the follower unloads the cell
        const EAST: u32 = 0x9733;
        let mut d = both_in_reach();
        d.loaded(C2, RIVERWOOD, 1_000);
        d.loaded(C2, EAST, 1_000);
        assert_eq!(d.may_move(&CellMove { sender: C2, actor: EMBRY, origin: RIVERWOOD, dest: EAST, adjacent: true, door: false, edge_units: u32::MAX }, 2_000), MoveVerdict::Follow);
        assert_eq!(d.unloaded(C2, EAST, 3_000), vec![]);
        assert_eq!(d.simulator_of(EMBRY, EAST), 0);
        assert_eq!(d.may_move(&CellMove { sender: C2, actor: EMBRY, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: u32::MAX }, 3_100), MoveVerdict::Refuse);
    }

    #[test]
    fn the_cold_starts_first_grants_pass_to_a_nearer_player_inside_the_dwell_and_stand_after_it() {
        // ADR-047 D10: after a restart two games reconnect 20 ms apart; the
        // cell each stands in goes to it, not to whoever reported first
        const WEST: u32 = 0x9731;
        let mut c = Cells::default();
        // c1 stands in Riverwood's square, west is its neighbour
        assert_eq!(c.reach_ranked(C1, &[RIVERWOOD, WEST], &[0, 1], 1_000), vec![]);
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C1, WEST, 1_001), vec![Decision::Grant { player: C1, cell: WEST, epoch: 1 }]);
        // c2 stands in the west square: west passes to it at once, Riverwood stays
        assert_eq!(c.loaded(C2, WEST, 1_020), vec![]);
        assert_eq!(c.loaded(C2, RIVERWOOD, 1_020), vec![]);
        assert_eq!(
            c.reach_ranked(C2, &[WEST, RIVERWOOD], &[0, 1], 1_021),
            vec![Decision::Release { player: C1, cell: WEST, epoch: 1 }, Decision::Grant { player: C2, cell: WEST, epoch: 2 }]
        );
        assert_eq!(c.host_of(RIVERWOOD), Some((C1, 1)));
        assert_eq!(c.host_of(WEST), Some((C2, 2)));
        // a tie in nearness stays with the arrival order
        assert_eq!(c.reach_ranked(C2, &[WEST, RIVERWOOD], &[0, 0], 1_030), vec![]);
        // after the dwell a nearer arrival takes nothing: hosts do not churn
        let mut d = Cells::default();
        d.reach_ranked(C1, &[RIVERWOOD], &[1], 1_000);
        assert_eq!(d.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        d.loaded(C2, RIVERWOOD, 1_000 + COLD_START_MS);
        assert_eq!(d.reach_ranked(C2, &[RIVERWOOD], &[0], 1_000 + COLD_START_MS), vec![]);
        assert_eq!(d.host_of(RIVERWOOD), Some((C1, 1)));
    }

    #[test]
    fn judging_a_move_records_nothing_and_applying_it_does() {
        const EAST: u32 = 0x9733;
        const STUMP: u32 = 0x1e62b;
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        c.loaded(C1, EAST, 1_100);
        let m = CellMove { sender: C1, actor: STUMP, origin: RIVERWOOD, dest: EAST, adjacent: true, door: false, edge_units: u32::MAX };
        assert_eq!(c.judge(&m, 9_000), MoveVerdict::Follow);
        assert_eq!(c.follows(), vec![]);
        assert_eq!(c.may_move(&m, 10_000), MoveVerdict::Follow);
        assert_eq!(c.follows(), vec![(STUMP, C1, EAST)]);
        // judging the way back removes nothing; applying it does (both past
        // the settle window, else the return is a stale sample)
        let back = CellMove { sender: C1, actor: STUMP, origin: EAST, dest: RIVERWOOD, adjacent: true, door: false, edge_units: u32::MAX };
        assert_eq!(c.judge(&back, 10_000 + FOLLOW_SETTLE_MS), MoveVerdict::Accept);
        assert_eq!(c.follows(), vec![(STUMP, C1, EAST)]);
        assert_eq!(c.may_move(&back, 10_000 + FOLLOW_SETTLE_MS + 500), MoveVerdict::Accept);
        assert_eq!(c.follows(), vec![]);
    }

    #[test]
    fn a_report_back_out_within_the_settle_after_a_door_follow_is_stale() {
        const INN: u32 = 0x133c6;
        const ALVOR: u32 = 0x13482;
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        let inward = CellMove { sender: C1, actor: ALVOR, origin: RIVERWOOD, dest: INN, adjacent: false, door: true, edge_units: u32::MAX };
        assert_eq!(c.may_move(&inward, 10_000), MoveVerdict::Follow);
        // the host's next sample, taken before the engine's transit, says the exterior
        let outward = CellMove { sender: C1, actor: ALVOR, origin: INN, dest: RIVERWOOD, adjacent: false, door: true, edge_units: u32::MAX };
        assert_eq!(c.may_move(&outward, 10_133), MoveVerdict::Refuse);
        assert_eq!(c.follows(), vec![(ALVOR, C1, INN)]);
        // a real return after the window passes and ends the follow
        assert_eq!(c.may_move(&outward, 10_000 + FOLLOW_SETTLE_MS), MoveVerdict::Accept);
        assert_eq!(c.follows(), vec![]);
    }

    #[test]
    fn a_crossing_within_the_margin_keeps_the_crossing_host_and_a_deeper_step_passes() {
        // ADR-047 D22's brake (2026-10-26): Alvor working the border between
        // 9731 and 9732 passed 836 times in one green run; the pass now waits
        // for the actor to stand PASS_MARGIN_UNITS inside the other host's cell
        const EAST: u32 = 0x9733;
        const SIGRID: u32 = 0x13483;
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C2, EAST, 1_100), vec![]);
        assert_eq!(c.reach(C2, &[RIVERWOOD, EAST], 1_200), vec![Decision::Grant { player: C2, cell: EAST, epoch: 1 }]);
        // a step over the line, 100 units in: the crossing host keeps her
        assert_eq!(
            c.may_move(&CellMove { sender: C1, actor: SIGRID, origin: RIVERWOOD, dest: EAST, adjacent: true, door: false, edge_units: 100 }, 2_000),
            MoveVerdict::Follow
        );
        assert_eq!(c.simulator_of(SIGRID, EAST), C1);
        assert_eq!(c.follows(), vec![(SIGRID, C1, EAST)]);
        // her reports stay c1's; c2's are not hers to make
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: SIGRID, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: 120 }, 2_200), MoveVerdict::Accept);
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: SIGRID, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: 120 }, 2_300), MoveVerdict::Refuse);
        // deep inside: the pass, the follow gone, c2 simulates from here
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: SIGRID, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: 300 }, 3_500), MoveVerdict::Pass);
        assert_eq!(c.follows(), vec![]);
        assert_eq!(c.simulator_of(SIGRID, EAST), C2);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: SIGRID, origin: EAST, dest: EAST, adjacent: false, door: false, edge_units: 300 }, 3_600), MoveVerdict::Refuse);
    }

    #[test]
    fn a_border_worked_back_and_forth_within_the_margin_passes_nothing() {
        const EAST: u32 = 0x9733;
        const ALVOR: u32 = 0x13482;
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C2, EAST, 1_100), vec![]);
        assert_eq!(c.reach(C2, &[RIVERWOOD, EAST], 1_200), vec![Decision::Grant { player: C2, cell: EAST, epoch: 1 }]);
        let mut passes = 0;
        let mut now = 2_000;
        let mut origin = RIVERWOOD;
        for _ in 0..20 {
            let dest = if origin == RIVERWOOD { EAST } else { RIVERWOOD };
            // past the settle window each time, so a return is a return
            now += FOLLOW_SETTLE_MS + 500;
            let v = c.may_move(&CellMove { sender: C1, actor: ALVOR, origin, dest, adjacent: true, door: false, edge_units: 40 }, now);
            assert_ne!(v, MoveVerdict::Refuse, "the crossing host's own report");
            if v == MoveVerdict::Pass {
                passes += 1;
            }
            assert_eq!(c.simulator_of(ALVOR, dest), C1, "the crossing host keeps him");
            origin = dest;
        }
        assert_eq!(passes, 0);
    }
}
