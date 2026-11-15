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

/// A dialogue claim's lease (ADR-052, thuum docs/verbs/npc-talk.md): a player
/// given an NPC for a conversation keeps it this long unless it releases it,
/// leaves the cell or the server, or the actor dies; a grant into the cell
/// passes over it (ADR-073 decision 6, D-L2 B). The lease is the talk
/// purpose's alone: a lead and a scene end on their own judged events, never
/// a timer (ADR-063). UNMEASURED.
pub const CLAIM_LEASE_MS: u64 = 120_000;

/// How many quests' leads one actor's claim row holds at once (ADR-073
/// decision 6 as D-53-4 amends it: a quest's own package on an actor that
/// fills one of its aliases is that quest's lead). An actor fills a few
/// quests' aliases at a time; a fifth quest's lead is refused
/// ([`ClaimVerdict::Full`]) rather than a held one dropped. A bound, not a
/// measurement.
pub const MAX_LEADS: usize = 4;

/// The alias a lead records when the server cannot tell which of its
/// quest's aliases holds the actor (alias_fills::ActorFill::Unresolved,
/// R2): no alias's refill ends that lead; its quest's stop, its holder's
/// release or the actor's death does.
pub const UNRESOLVED_ALIAS: u32 = u32::MAX;

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
    /// A player's claim row on `actor` in `cell` ended with its last purpose
    /// (ADR-052, ADR-073 decision 6): the cell's rule names the simulator
    /// again, the word follows.
    ClaimEnded {
        /// The actor.
        actor: u32,
        /// The cell its claim named.
        cell: u32,
        /// The holder, its simulator until the end (the core's word skips a
        /// game the cell's rule names again).
        player: u32,
        /// The actor's owner epoch after the end (ADR-066 decision 1).
        epoch: u32,
        /// What ended it.
        why: ClaimEnd,
    },
}

/// What ended a claim row (ADR-073 decision 6: each kind declares its own end
/// events), named in the core's log and carried as the bridge's code.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ClaimEnd {
    /// The talk's lease ran out (CLAIM_LEASE_MS).
    Lease = 0,
    /// The holder released its last purpose.
    Release = 1,
    /// The quest of its leads, or of its scene, stopped.
    QuestStop = 2,
    /// The alias its lead read now names another reference, or none.
    Alias = 3,
    /// The actor died.
    Death = 4,
    /// The holder left the server.
    Gone = 5,
    /// The holder's connection stayed silent past GRACE_MS: its leads and its
    /// scene end, as its cells pass on.
    Stale = 6,
    /// The holder unloaded the actor's cell: a talk's end (ADR-052 decision 2).
    Unloaded = 7,
    /// The holder's game stopped running the actor on the lead's quest, its
    /// own report (D-53-5 B, R2): a lead's end.
    NotRunning(NotRunning) = 8,
}

/// How a holder's game stopped running its lead's actor (D-53-5 B, D-PO2 A's
/// reading: the running game claims her while it runs her), each the
/// holder's own report (R2): the server records it and cannot check it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NotRunning {
    /// The package the holder's engine started on her is not one of the
    /// lead's quest's own packages (its QNAM another quest, or none).
    PackageChanged,
    /// The actor is no longer loaded in the holder's game (her 3D unloaded
    /// there, or the game holds her no more).
    Unloaded,
}

impl ClaimEnd {
    /// The word the core logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            ClaimEnd::Lease => "its lease",
            ClaimEnd::Release => "its release",
            ClaimEnd::QuestStop => "its quest's stop",
            ClaimEnd::Alias => "its alias's fill",
            ClaimEnd::Death => "the actor's death",
            ClaimEnd::Gone => "its holder's departure",
            ClaimEnd::Stale => "its holder's silence",
            ClaimEnd::Unloaded => "its holder's unload",
            ClaimEnd::NotRunning(NotRunning::PackageChanged) => "its holder's game leaving the quest's own packages",
            ClaimEnd::NotRunning(NotRunning::Unloaded) => "its holder's game unloading the actor",
        }
    }
}

/// What a claim holds its actor for (ADR-073 decision 6; thuum
/// docs/verbs/npc-talk.md): one claim row per actor, holding one or more of
/// these, ending when its last one ends.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Purpose {
    /// A dialogue (ADR-052): for CLAIM_LEASE_MS unless released, the
    /// claimant's unload or departure, or the actor's death.
    Talk,
    /// A quest's own package (its QNAM the quest) running on an actor that
    /// fills one of the quest's aliases (D-53-4, amending decision 6's lead):
    /// the quest's claim, until the quest releases the actor (its alias names
    /// another, or the holder's release), stops, or the actor dies, or the
    /// holder's game stops running her (D-53-5 B: another quest's package or
    /// none, her unload there, the holder's departure or silence). Only the
    /// actor's simulator opens one (D-PO2 A, "the running game claims it").
    Lead {
        /// The QUST.
        quest: u32,
        /// The quest's alias the actor fills, the server's own read.
        alias: u32,
    },
    /// A scene the claimant's seat started (D-L2 B; D-A2 C, the stage's
    /// cause seat for a scene a fragment starts): until it ends (the holder's
    /// release) or its quest stops.
    Scene {
        /// The SCEN.
        scene: u32,
        /// The scene's own quest (its PNAM outside an action).
        quest: u32,
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
    /// Whether the move passes a door: an interior at either end, any actor's
    /// (memo D54 B, 2026-10-28; the core asked the actor's persistence here
    /// until then, and Riverwood's one resident placed without the flag paced
    /// at her own door).
    pub door: bool,
    /// How far the reported position stands inside the destination's exterior
    /// square, in units from its nearest edge (u32::MAX for an interior or an
    /// unknown): the pass's hysteresis reads it (PASS_MARGIN_UNITS).
    pub edge_units: u32,
}

/// Why a player simulates one actor against the cell's rule: a follow into a
/// cell nobody hosts (ADR-047 D6), the pass margin's hold in another host's
/// cell (D22), or a claim a player asked for, for a dialogue, a quest's lead
/// or a scene (ADR-052, ADR-073 decision 6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FollowKind {
    /// Into a cell nobody hosts (ADR-047 D6).
    Follow,
    /// The pass margin's hold in another host's cell (ADR-047 D22).
    Margin,
    /// A player's claim, its purposes on the row (ADR-052, ADR-073 decision
    /// 6): it travels with its actor and outlives a grant into its cell.
    Claim,
}

/// A host simulating an actor in a cell nobody hosts (ADR-047 D6), or, within
/// the pass margin, in another host's cell it has just crossed into (D22), or
/// a player holding an actor it claimed (ADR-052, ADR-073 decision 6): the
/// owner table ADR-050 decision 4 named, one row per actor. A claim row holds
/// at least one purpose while it stands.
#[derive(Debug, Clone, PartialEq, Eq)]
struct Follow {
    player: u32,
    cell: u32,
    since_ms: u64,
    kind: FollowKind,
    /// A claim's talk: its lease's end; none for a claim held for a lead or a
    /// scene alone, and for the other kinds.
    lease_until_ms: Option<u64>,
    /// A claim's leads, (quest, alias), at most MAX_LEADS.
    leads: Vec<(u32, u32)>,
    /// A claim's scene, (scene, quest).
    scene: Option<(u32, u32)>,
    /// A claim's owner epoch at its grant (ADR-066 decision 1, the actor's
    /// counter); 0 for the other kinds.
    epoch: u32,
}

impl Follow {
    /// A row of `kind` with no purpose yet: a follow, a margin hold, or the
    /// claim a purpose is about to join.
    fn moved(player: u32, cell: u32, since_ms: u64, kind: FollowKind) -> Follow {
        Follow { player, cell, since_ms, kind, lease_until_ms: None, leads: Vec::new(), scene: None, epoch: 0 }
    }

    /// A claim row whose last purpose ended.
    fn holds_nothing(&self) -> bool {
        self.lease_until_ms.is_none() && self.leads.is_empty() && self.scene.is_none()
    }

    /// `purpose` joins the row: Reclaimed when it held the same one (a talk's
    /// lease renewed, a lead of the same quest, the same scene), Granted for a
    /// new one, Full for a lead past MAX_LEADS.
    fn join(&mut self, purpose: Purpose, now_ms: u64) -> ClaimVerdict {
        match purpose {
            Purpose::Talk => {
                let renewed = self.lease_until_ms.is_some();
                self.lease_until_ms = Some(now_ms.saturating_add(CLAIM_LEASE_MS));
                if renewed {
                    ClaimVerdict::Reclaimed
                } else {
                    ClaimVerdict::Granted
                }
            }
            Purpose::Lead { quest, alias } => {
                if let Some(held) = self.leads.iter_mut().find(|(q, _)| *q == quest) {
                    held.1 = alias;
                    ClaimVerdict::Reclaimed
                } else if self.leads.len() >= MAX_LEADS {
                    ClaimVerdict::Full
                } else {
                    self.leads.push((quest, alias));
                    ClaimVerdict::Granted
                }
            }
            Purpose::Scene { scene, quest } => {
                let same = self.scene.is_some_and(|(s, _)| s == scene);
                self.scene = Some((scene, quest));
                if same {
                    ClaimVerdict::Reclaimed
                } else {
                    ClaimVerdict::Granted
                }
            }
        }
    }
}

/// The answer to a claim (ADR-052, ADR-073 decision 6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ClaimVerdict {
    /// The claimant simulates the actor from now, for the purpose's life: a
    /// new row, or a new purpose joining the claimant's own.
    Granted,
    /// Another player holds a claim on the actor, for any purpose: the first
    /// holder keeps it.
    Held,
    /// A talk or a scene from a claimant that has not loaded the actor's cell.
    Unloaded,
    /// The claimant held the same purpose already (a talk's lease renewed).
    Reclaimed,
    /// A lead from a player that does not simulate the actor where it stands.
    NotSimulator,
    /// A lead past the row's MAX_LEADS quests.
    Full,
}

/// What a release did (ADR-052, ADR-073 decision 6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Release {
    /// The player holds no such purpose on the actor.
    NotHeld,
    /// The purpose ended and the row stands for the others: the holder still
    /// simulates the actor.
    Kept,
    /// The row's last purpose ended: its ClaimEnded.
    Ended(Decision),
}

/// One claim row, for the lab's reading and the core's log.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClaimRow {
    /// The actor.
    pub actor: u32,
    /// The holder.
    pub player: u32,
    /// The cell its row names.
    pub cell: u32,
    /// It holds a talk.
    pub talk: bool,
    /// Its leads, (quest, alias).
    pub leads: Vec<(u32, u32)>,
    /// Its scene, (scene, quest).
    pub scene: Option<(u32, u32)>,
    /// Its owner epoch at the grant (ADR-066 decision 1).
    pub epoch: u32,
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
    /// When the connection went silent (the first `stale` since it was last
    /// heard): its claims' leads and scene end GRACE_MS after it, as its
    /// cells pass on (ADR-073 decision 6).
    stale_since_ms: Option<u64>,
    /// The player's actor is dead (thuum docs/research/m3/
    /// decisions-after-playtest-34.md D18): no candidate anywhere, its
    /// hosted cells in their grace from the death, its loaded set kept for
    /// the respawn; the server's own number, not the connection's.
    dead: bool,
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
    /// Each claimed actor's owner epoch (ADR-066 decision 1): up at every
    /// claim row's grant and at its end; ADR-073 step 5b carries it on the
    /// word and stamps the other changes of simulator.
    owner_epochs: HashMap<u32, u32>,
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
    /// hosts the cell; otherwise it only stops being a candidate. Its follows
    /// into the cell end, and its claims' talks there (a claim row with no
    /// purpose left ends, its word first); a lead or a scene stays.
    pub fn unloaded(&mut self, player: u32, cell: u32, now_ms: u64) -> Vec<Decision> {
        if let Some(p) = self.players.get_mut(&player) {
            p.loaded.remove(&cell);
        }
        // a follow into an exterior the follower no longer loads ends (ADR-047
        // D6), and so does a claim's talk (the claimant leaving the cell,
        // ADR-052 decision 2); a lead or a scene stays, since its holder's
        // engine runs the actor in a cell its game has unloaded (R-C2: rotfern
        // inside the trader while its game walked Camilla out of the door)
        self.follows.retain(|_, f| f.kind == FollowKind::Claim || !(f.player == player && f.cell == cell));
        let mut out = self.end_purposes(ClaimEnd::Unloaded, |_, f| {
            if f.player == player && f.cell == cell {
                f.lease_until_ms = None;
            }
        });
        out.extend(self.consider(player, cell, now_ms));
        out
    }

    /// `player` left the server at `now_ms`: every cell it hosts starts its
    /// grace, and it is no candidate anywhere. Its follows go with it, and
    /// each claim row it held ends whole, a ClaimEnded apiece (by actor id),
    /// so the word names the cell's rule again.
    pub fn gone(&mut self, player: u32, now_ms: u64) -> Vec<Decision> {
        self.players.remove(&player);
        self.follows.retain(|_, f| f.kind == FollowKind::Claim || f.player != player);
        let out = self.end_purposes(ClaimEnd::Gone, |_, f| {
            if f.player == player {
                f.lease_until_ms = None;
                f.leads.clear();
                f.scene = None;
            }
        });
        for state in self.cells.values_mut() {
            if let Some(ref mut host) = state.host {
                if host.player == player && host.leaving_since_ms.is_none() {
                    host.leaving_since_ms = Some(now_ms);
                }
            }
        }
        out
    }

    /// `player`'s connection went silent at `now_ms` (the game killed,
    /// crashed or quit while netcode's own timeout still runs): every cell
    /// it hosts starts its grace and it is no candidate anywhere, but its
    /// loaded set and reach stay, so a return within netcode's patience is
    /// `revived` and not a fresh arrival.
    pub fn stale(&mut self, player: u32, now_ms: u64) -> Vec<Decision> {
        let p = self.players.entry(player).or_default();
        p.stale = true;
        p.stale_since_ms.get_or_insert(now_ms);
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
        p.stale_since_ms = None;
        let mut cells: Vec<u32> = p.loaded.keys().copied().collect();
        cells.sort_unstable();
        cells.into_iter().flat_map(|cell| self.consider(player, cell, now_ms)).collect()
    }

    /// `player`'s actor died at `now_ms` (thuum docs/research/m3/
    /// decisions-after-playtest-34.md D18): every cell it hosts starts its
    /// grace and it is no candidate anywhere until `respawned`; its loaded
    /// set and reach stay, since its game keeps the cell loaded while the
    /// body lies.
    pub fn died(&mut self, player: u32, now_ms: u64) -> Vec<Decision> {
        self.players.entry(player).or_default().dead = true;
        for state in self.cells.values_mut() {
            if let Some(ref mut host) = state.host {
                if host.player == player && host.leaving_since_ms.is_none() {
                    host.leaving_since_ms = Some(now_ms);
                }
            }
        }
        Vec::new()
    }

    /// `player`'s actor stood again at `now_ms`: a candidate again; each cell
    /// it has loaded is considered anew, which disarms its own grace where
    /// it still hosts and grants a free cell the grid gives it.
    pub fn respawned(&mut self, player: u32, now_ms: u64) -> Vec<Decision> {
        let Some(p) = self.players.get_mut(&player) else {
            return Vec::new();
        };
        p.dead = false;
        let mut cells: Vec<u32> = p.loaded.keys().copied().collect();
        cells.sort_unstable();
        cells.into_iter().flat_map(|cell| self.consider(player, cell, now_ms)).collect()
    }

    /// Whether `player` has `cell` loaded and in reach, is heard from and
    /// alive: what hosting takes.
    fn eligible(&self, player: u32, cell: u32) -> bool {
        self.players
            .get(&player)
            .is_some_and(|p| !p.stale && !p.dead && p.reach.contains_key(&cell) && p.loaded.contains_key(&cell))
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

    /// `player` hosts `cell` from `now_ms` at the next epoch; the follow and
    /// margin rows into the cell end with the grant, and a claim passes over
    /// it (ADR-073 decision 6, D-L2 B: a claim outlives a grant for its life).
    fn grant(&mut self, player: u32, cell: u32, now_ms: u64) -> Decision {
        // a grant into the cell ends its follow and margin rows (the host
        // simulates them now); a claim (a talk, a lead, a scene) outranks the
        // host and stays, whoever the grant names (playtest 36, 2026-10-28: a
        // regrant of 9732 to the claimant itself dropped its claim on Sven,
        // and its release met E_CLAIM_NONE; to a third player it would have
        // flipped the actor away mid-dialogue; R-C2, playtest 52: the grant of
        // 9731 to ff000001 dropped rotfern's follow of Camilla on her lead,
        // and every report of its game was refused from then)
        self.follows.retain(|_, f| f.cell != cell || f.kind == FollowKind::Claim);
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
        // a row names the simulator first: a follow into a cell nobody hosts
        // (D6) or, within the pass margin, into another host's cell the
        // follower has just crossed into (D22), either ended by a grant into
        // the cell; or a claim (ADR-052, ADR-073 decision 6), which a grant
        // passes over
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
    /// crossing into another live host's cell, Pass (D9), and its door into
    /// one too (Eli, playtest 37, 2026-10-28: "let embry cross and hand him
    /// to the inn's host. that's a good rule to have"); anything else
    /// Refuse. The
    /// door's pre-check reads this (x-handoff-door 20261024-001915: a verdict
    /// that wrote follow rows at the door removed a standing follow before
    /// the host was looked up, and recorded one before the host had crossed).
    #[must_use]
    pub fn judge(&self, m: &CellMove, now_ms: u64) -> MoveVerdict {
        let CellMove { sender, actor, origin, dest, adjacent, door, edge_units } = *m;
        // a claim travels with its actor (ADR-052; playtest 35, 2026-10-27:
        // Hilde walked from 9732 into 9731 under Prisoner's claim, the Follow
        // verdict made the row a margin follow and a step deeper a pass, and
        // the claim ended with no word, the client's flag never cleared): the
        // claimant's report into any cell is accepted, the row keeps its kind
        // and its purposes and names the new cell (may_move); a lead's holder
        // walks her through a door into a cell another seat hosts with no pass
        // while she is claimed (R-C2)
        if dest != 0 && self.follows.get(&actor).is_some_and(|f| f.kind == FollowKind::Claim && f.player == sender) {
            return MoveVerdict::Accept;
        }
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
                .is_some_and(|f| f.cell == dest && f.player == sender && f.kind == FollowKind::Margin)
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
                // teleport across squares or a stranger's report is dropped.
                // A door is a crossing too (Eli, playtest 37, 2026-10-28:
                // Embry stood at the Sleeping Giant's door while Prisoner
                // hosted the inn, his host's engine walking him back to it
                // every few seconds): the simulator's actor through a door
                // into a cell another seat hosts passes to that host, so its
                // schedule goes on indoors (any actor's door, memo D54 B); an
                // interior is deep by definition, an exit lands under the
                // margin's brake
                if simulator == sender && (adjacent || door) {
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
                // into a cell nobody hosts (D6), or another host's within the margin (D22)
                let kind = if self.host_of(m.dest).is_some() { FollowKind::Margin } else { FollowKind::Follow };
                self.follows.insert(m.actor, Follow::moved(m.sender, m.dest, now_ms, kind));
            }
            MoveVerdict::Accept if m.dest != m.origin => {
                // a claim follows its actor into the new cell (ADR-052)
                if let Some(f) = self.follows.get_mut(&m.actor) {
                    if f.kind == FollowKind::Claim && f.player == m.sender {
                        f.cell = m.dest;
                        return verdict;
                    }
                }
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

    /// `player` claims `actor`, which stands in `cell`, for `purpose`
    /// (ADR-052, ADR-073 decision 6). A talk and a scene want the cell loaded
    /// by the claimant; a lead wants the claimant to be the actor's simulator
    /// there (D-PO2 A), loaded or not. Another player's claim of any purpose
    /// holds the actor (the first holder keeps it); the claimant's own row
    /// takes the purpose beside its others; a follow or margin row gives way.
    /// A new row is the actor's next owner epoch. The row names the claimant
    /// as the simulator (`simulator_of`) until its last purpose ends.
    pub fn claim(&mut self, actor: u32, player: u32, cell: u32, purpose: Purpose, now_ms: u64) -> ClaimVerdict {
        let lead = matches!(purpose, Purpose::Lead { .. });
        // the simulator may run a lead's actor in a cell its game has
        // unloaded (R-C2); a talk and a scene are the cell's visitors'
        if !lead && !self.players.get(&player).is_some_and(|p| p.loaded.contains_key(&cell)) {
            return ClaimVerdict::Unloaded;
        }
        match self.follows.get_mut(&actor) {
            Some(f) if f.kind == FollowKind::Claim && f.player != player => return ClaimVerdict::Held,
            Some(f) if f.kind == FollowKind::Claim => {
                let verdict = f.join(purpose, now_ms);
                if verdict != ClaimVerdict::Full {
                    f.cell = cell;
                }
                return verdict;
            }
            _ => {}
        }
        if lead && self.simulator_of(actor, cell) != player {
            return ClaimVerdict::NotSimulator;
        }
        let mut row = Follow::moved(player, cell, now_ms, FollowKind::Claim);
        row.epoch = self.bump(actor);
        let verdict = row.join(purpose, now_ms);
        self.follows.insert(actor, row);
        verdict
    }

    /// `player` releases `purpose` of its claim on `actor`: a talk, the lead
    /// of the purpose's quest, or the purpose's scene (the other fields are
    /// not read). The row stands while another purpose holds it; its last
    /// one's release ends it.
    pub fn release_claim(&mut self, actor: u32, player: u32, purpose: Purpose) -> Release {
        self.release_for(actor, player, purpose, ClaimEnd::Release)
    }

    /// `player`'s game stopped running `actor` on its lead of `quest` (D-53-5
    /// B), `how` its own report (R2): that lead ends as its release does, and
    /// the actor returns to its cell's rule (its host, or nobody) once the
    /// row holds nothing else; a lead it does not hold is none. Judged from
    /// the report, never a timer (ADR-063); a holder that stops reporting is
    /// `tick`'s Stale end, one that leaves `gone`'s.
    pub fn not_running(&mut self, actor: u32, player: u32, quest: u32, how: NotRunning) -> Release {
        self.release_for(actor, player, Purpose::Lead { quest, alias: 0 }, ClaimEnd::NotRunning(how))
    }

    /// The holder's end of one purpose, `why` its ClaimEnded's.
    fn release_for(&mut self, actor: u32, player: u32, purpose: Purpose, why: ClaimEnd) -> Release {
        let Some(f) = self.follows.get_mut(&actor) else {
            return Release::NotHeld;
        };
        if f.kind != FollowKind::Claim || f.player != player {
            return Release::NotHeld;
        }
        let held = match purpose {
            Purpose::Talk => f.lease_until_ms.take().is_some(),
            Purpose::Lead { quest, .. } => {
                let before = f.leads.len();
                f.leads.retain(|(q, _)| *q != quest);
                f.leads.len() != before
            }
            Purpose::Scene { scene, .. } => {
                let held = f.scene.is_some_and(|(s, _)| s == scene);
                if held {
                    f.scene = None;
                }
                held
            }
        };
        if !held {
            return Release::NotHeld;
        }
        if !f.holds_nothing() {
            return Release::Kept;
        }
        let (cell, player) = (f.cell, f.player);
        self.follows.remove(&actor);
        let epoch = self.bump(actor);
        Release::Ended(Decision::ClaimEnded { actor, cell, player, epoch, why })
    }

    /// `quest` stopped (the world's record took its stop): its leads end, and
    /// the scenes whose quest it is.
    pub fn quest_stopped(&mut self, quest: u32) -> Vec<Decision> {
        self.end_purposes(ClaimEnd::QuestStop, |_, f| {
            f.leads.retain(|(q, _)| *q != quest);
            if f.scene.is_some_and(|(_, q)| q == quest) {
                f.scene = None;
            }
        })
    }

    /// `quest`'s alias `alias` now names `refr` (0 a clear): a lead that read
    /// the alias on any other actor ends (the quest released her, D-53-4); a
    /// lead at UNRESOLVED_ALIAS read no alias, and no refill ends it.
    pub fn alias_filled(&mut self, quest: u32, alias: u32, refr: u32) -> Vec<Decision> {
        self.end_purposes(ClaimEnd::Alias, |actor, f| {
            if actor != refr {
                f.leads.retain(|&(q, a)| a == UNRESOLVED_ALIAS || !(q == quest && a == alias));
            }
        })
    }

    /// `actor` died: its claim row ends whole (ADR-052 decision 2, D-53-4).
    pub fn actor_died(&mut self, actor: u32) -> Vec<Decision> {
        self.end_purposes(ClaimEnd::Death, |a, f| {
            if a == actor {
                f.lease_until_ms = None;
                f.leads.clear();
                f.scene = None;
            }
        })
    }

    /// Whether `player` holds a lead or a scene on any actor: a seat whose
    /// silence the core must judge though it hosts no cell.
    #[must_use]
    pub fn holds_lead_or_scene(&self, player: u32) -> bool {
        self.follows.values().any(|f| f.kind == FollowKind::Claim && f.player == player && (!f.leads.is_empty() || f.scene.is_some()))
    }

    /// `actor`'s owner epoch (ADR-066 decision 1): 0 before its first claim.
    #[must_use]
    pub fn owner_epoch_of(&self, actor: u32) -> u32 {
        self.owner_epochs.get(&actor).copied().unwrap_or(0)
    }

    /// The actor's next owner epoch, kept.
    fn bump(&mut self, actor: u32) -> u32 {
        let epoch = self.owner_epochs.entry(actor).or_insert(0);
        *epoch = epoch.saturating_add(1);
        *epoch
    }

    /// Ends, on every claim row, the purposes `strip` removes (it is given
    /// the actor and the row); each row left with none goes, its actor's
    /// epoch up, as one ClaimEnded `why`, by actor id.
    fn end_purposes(&mut self, why: ClaimEnd, mut strip: impl FnMut(u32, &mut Follow)) -> Vec<Decision> {
        let mut ended: Vec<(u32, u32, u32)> = Vec::new();
        for (&actor, f) in &mut self.follows {
            if f.kind != FollowKind::Claim {
                continue;
            }
            strip(actor, f);
            if f.holds_nothing() {
                ended.push((actor, f.cell, f.player));
            }
        }
        ended.sort_unstable();
        ended
            .into_iter()
            .map(|(actor, cell, player)| {
                self.follows.remove(&actor);
                let epoch = self.bump(actor);
                Decision::ClaimEnded { actor, cell, player, epoch, why }
            })
            .collect()
    }

    /// Every claim, by actor id: the actor, its claimant and the cell.
    #[must_use]
    pub fn claims(&self) -> Vec<(u32, u32, u32)> {
        let mut rows: Vec<(u32, u32, u32)> =
            self.follows.iter().filter(|(_, f)| f.kind == FollowKind::Claim).map(|(&a, f)| (a, f.player, f.cell)).collect();
        rows.sort_unstable();
        rows
    }

    /// Every claim row with its purposes and epoch, by actor id (the lab's
    /// reading, the core's log).
    #[must_use]
    pub fn claim_rows(&self) -> Vec<ClaimRow> {
        let mut rows: Vec<ClaimRow> = self
            .follows
            .iter()
            .filter(|(_, f)| f.kind == FollowKind::Claim)
            .map(|(&actor, f)| ClaimRow {
                actor,
                player: f.player,
                cell: f.cell,
                talk: f.lease_until_ms.is_some(),
                leads: f.leads.clone(),
                scene: f.scene,
                epoch: f.epoch,
            })
            .collect();
        rows.sort_unstable_by_key(|r| r.actor);
        rows
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
        // a talk past its lease ends (ADR-052); a lead or a scene ends on its
        // own events, never a timer (ADR-063)
        let mut out = self.end_purposes(ClaimEnd::Lease, |_, f| {
            if f.lease_until_ms.is_some_and(|t| now_ms >= t) {
                f.lease_until_ms = None;
            }
        });
        // a holder silent past the grace loses its leads and its scene, the
        // judgment its cells get (a dead holder keeps them: its engine still
        // runs the actor)
        let silent: Vec<u32> = self
            .players
            .iter()
            .filter(|(_, p)| p.stale_since_ms.is_some_and(|t| now_ms.saturating_sub(t) >= GRACE_MS))
            .map(|(&id, _)| id)
            .collect();
        if !silent.is_empty() {
            out.extend(self.end_purposes(ClaimEnd::Stale, |_, f| {
                if silent.contains(&f.player) {
                    f.leads.clear();
                    f.scene = None;
                }
            }));
        }
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
            .filter(|&(&id, p)| {
                id != leaving && !p.stale && !p.dead && p.reach.contains_key(&cell) && p.loaded.contains_key(&cell)
            })
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

    /// Every player whose game reports `cell` loaded, lowest id first,
    /// whether or not the grid gives it the cell (a silent or a dead
    /// player's game keeps it loaded): the games that show what stands in
    /// it. thuum ADR-068: the listeners of a script call's subject the
    /// server holds no reference for, placed by its record.
    #[must_use]
    pub fn loaders_of(&self, cell: u32) -> Vec<u32> {
        let mut players: Vec<u32> = self.players.iter().filter(|(_, p)| p.loaded.contains_key(&cell)).map(|(&id, _)| id).collect();
        players.sort_unstable();
        players
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
    fn a_cells_loaders_are_the_games_that_report_it_loaded() {
        // thuum ADR-068: who a script call on a placement the server holds
        // no reference for is mirrored to
        let mut c = Cells::default();
        assert!(c.loaders_of(RIVERWOOD).is_empty());
        // a load the grid does not give counts: the game shows the cell
        c.loaded(C2, RIVERWOOD, 1_000);
        c.reach(C1, &[RIVERWOOD], 1_000);
        c.loaded(C1, RIVERWOOD, 1_100);
        assert_eq!(c.loaders_of(RIVERWOOD), vec![C1, C2]);
        assert!(c.loaders_of(0x1234).is_empty());
        // a dead player's game keeps the cell loaded; an unload and a
        // departure end it
        c.died(C2, 2_000);
        assert_eq!(c.loaders_of(RIVERWOOD), vec![C1, C2]);
        c.unloaded(C2, RIVERWOOD, 3_000);
        assert_eq!(c.loaders_of(RIVERWOOD), vec![C1]);
        c.gone(C1, 4_000);
        assert!(c.loaders_of(RIVERWOOD).is_empty());
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
    fn a_dead_host_loses_its_cell_at_the_grace_and_a_respawned_one_is_a_candidate_again() {
        // thuum docs/research/m3/decisions-after-playtest-34.md D18 (b-cell-host
        // 20261025-115250: c1 took a cell at epoch 2 while dead): the death is
        // the server's own number, so the grace starts there and the dead
        // player's own loaded report grants it nothing
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C2, RIVERWOOD, 1_500), vec![]);
        assert_eq!(c.died(C1, 10_000), vec![]);
        assert_eq!(c.tick(10_000 + GRACE_MS - 100), vec![]);
        assert_eq!(c.host_of(RIVERWOOD), Some((C1, 1)));
        // the grace runs out: the live visitor takes it
        assert_eq!(
            c.tick(10_000 + GRACE_MS),
            vec![
                Decision::Release { player: C1, cell: RIVERWOOD, epoch: 1 },
                Decision::Grant { player: C2, cell: RIVERWOOD, epoch: 2 }
            ]
        );
        // the new host leaves while the old one lies dead with the cell
        // loaded: nobody takes it
        assert_eq!(c.unloaded(C2, RIVERWOOD, 20_000), vec![]);
        assert_eq!(
            c.tick(20_000 + GRACE_MS),
            vec![Decision::Release { player: C2, cell: RIVERWOOD, epoch: 2 }, Decision::Unhosted { cell: RIVERWOOD }]
        );
        assert_eq!(c.host_of(RIVERWOOD), None);
        // a dead player's own loaded report grants it nothing
        assert_eq!(c.loaded(C1, RIVERWOOD, 30_000), vec![]);
        assert_eq!(c.host_of(RIVERWOOD), None);
        // it stands again: a candidate, and the free cell it has loaded is its
        assert_eq!(c.respawned(C1, 40_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 3 }]);
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
    fn a_door_into_another_live_hosts_cell_passes_the_actor_to_that_host() {
        // Eli, playtest 37 (2026-10-28): "let embry cross and hand him to the
        // inn's host. that's a good rule to have". Prisoner (C2) hosts the
        // Sleeping Giant from inside it, rotfern (C1) hosts Riverwood and
        // simulates Embry; his door into the inn passes him to C2 at once (an
        // interior is deep by definition), no follow left behind
        const INN: u32 = 0x133c6;
        const EMBRY: u32 = 0x3550c;
        let mut c = both_in_reach();
        c.reach(C2, &[RIVERWOOD, INN], 0);
        c.loaded(C1, RIVERWOOD, 1_000);
        c.loaded(C2, INN, 1_000);
        assert_eq!(c.host_of(INN), Some((C2, 1)));
        assert_eq!(c.simulator_of(EMBRY, RIVERWOOD), C1);
        let inward = CellMove { sender: C1, actor: EMBRY, origin: RIVERWOOD, dest: INN, adjacent: false, door: true, edge_units: u32::MAX };
        assert_eq!(c.judge(&inward, 2_000), MoveVerdict::Pass);
        assert_eq!(c.may_move(&inward, 2_000), MoveVerdict::Pass);
        assert_eq!(c.simulator_of(EMBRY, INN), C2);
        assert_eq!(c.follows(), vec![]);
        // the old host's report from indoors is a stranger's now
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: EMBRY, origin: INN, dest: INN, adjacent: false, door: false, edge_units: u32::MAX }, 2_100), MoveVerdict::Refuse);
        // a stranger pushing an actor through a door into its own cell is refused as before
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: 0x1348a, origin: RIVERWOOD, dest: INN, adjacent: false, door: true, edge_units: u32::MAX }, 2_200), MoveVerdict::Refuse);
        // and the exit by schedule: the inn's host walks him out into C1's cell, which takes him back
        let outward = CellMove { sender: C2, actor: EMBRY, origin: INN, dest: RIVERWOOD, adjacent: false, door: true, edge_units: u32::MAX };
        assert_eq!(c.may_move(&outward, 9_000), MoveVerdict::Pass);
        assert_eq!(c.simulator_of(EMBRY, RIVERWOOD), C1);
        // an exit landing within the pass margin of the exterior square stays the inn host's follow (D22's brake)
        let mut d = both_in_reach();
        d.reach(C2, &[RIVERWOOD, INN], 0);
        d.loaded(C1, RIVERWOOD, 1_000);
        d.loaded(C2, INN, 1_000);
        assert_eq!(d.may_move(&inward, 2_000), MoveVerdict::Pass);
        let edge = CellMove { sender: C2, actor: EMBRY, origin: INN, dest: RIVERWOOD, adjacent: false, door: true, edge_units: PASS_MARGIN_UNITS - 1 };
        assert_eq!(d.may_move(&edge, 9_000), MoveVerdict::Follow);
        assert_eq!(d.simulator_of(EMBRY, RIVERWOOD), C2);
    }

    #[test]
    fn the_door_fact_alone_vouches_for_any_actor_at_a_door() {
        // memo D54 B (Eli, 2026-10-28): the core's door fact asked the actor's
        // persistence until then, so Hilde 0x35534, Riverwood's one resident
        // placed without the flag, was refused at her own door into
        // RiverwoodSvensHouse 0x1cb84 and paced before it while her host's
        // engine walked her back. The table never saw persistence; these are
        // the facts the core sends for her now: her door into a house nobody
        // hosts is her host's follow (the record moves, the host holds her
        // there), a player loading the house takes her with the grant, a door
        // into a house another seat hosts is the pass, and without the door
        // fact a move into an interior stays refused
        const HOUSE: u32 = 0x1cb84;
        const HILDE: u32 = 0x35534;
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: HILDE, origin: RIVERWOOD, dest: HOUSE, adjacent: false, door: false, edge_units: u32::MAX }, 2_000), MoveVerdict::Refuse);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: HILDE, origin: RIVERWOOD, dest: HOUSE, adjacent: false, door: true, edge_units: u32::MAX }, 2_000), MoveVerdict::Follow);
        assert_eq!(c.simulator_of(HILDE, HOUSE), C1);
        assert_eq!(c.follows(), vec![(HILDE, C1, HOUSE)]);
        // frozen for every other game: a stranger's report from the house is refused, her host's passes
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: HILDE, origin: HOUSE, dest: HOUSE, adjacent: false, door: false, edge_units: u32::MAX }, 2_100), MoveVerdict::Refuse);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: HILDE, origin: HOUSE, dest: HOUSE, adjacent: false, door: false, edge_units: u32::MAX }, 2_200), MoveVerdict::Accept);
        // a player loads the house: the grant ends the follow and names the simulator
        c.reach(C2, &[RIVERWOOD, HOUSE], 2_900);
        assert_eq!(c.loaded(C2, HOUSE, 3_000), vec![Decision::Grant { player: C2, cell: HOUSE, epoch: 1 }]);
        assert_eq!(c.simulator_of(HILDE, HOUSE), C2);
        assert_eq!(c.follows(), vec![]);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: HILDE, origin: HOUSE, dest: HOUSE, adjacent: false, door: false, edge_units: u32::MAX }, 3_100), MoveVerdict::Refuse);
        // a door into a house another seat hosts from the start: the pass
        let mut d = both_in_reach();
        d.reach(C2, &[RIVERWOOD, HOUSE], 0);
        d.loaded(C1, RIVERWOOD, 1_000);
        d.loaded(C2, HOUSE, 1_000);
        assert_eq!(d.may_move(&CellMove { sender: C1, actor: HILDE, origin: RIVERWOOD, dest: HOUSE, adjacent: false, door: true, edge_units: u32::MAX }, 2_000), MoveVerdict::Pass);
        assert_eq!(d.simulator_of(HILDE, HOUSE), C2);
        assert_eq!(d.follows(), vec![]);
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

    #[test]
    fn a_talk_gives_one_actor_to_the_claimant_until_its_release_or_its_lease_and_outlives_a_grant() {
        // ADR-052 (thuum docs/verbs/npc-talk.md): a guest in the host's cell
        // asks for Sven for a dialogue
        const SVEN: u32 = 0x13491;
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C2, RIVERWOOD, 1_100), vec![]);
        assert_eq!(c.simulator_of(SVEN, RIVERWOOD), C1);
        // the claim: c2 simulates him; his reports are c2's now, c1's refused
        assert_eq!(c.claim(SVEN, C2, RIVERWOOD, Purpose::Talk, 2_000), ClaimVerdict::Granted);
        assert_eq!(c.simulator_of(SVEN, RIVERWOOD), C2);
        assert_eq!(c.claims(), vec![(SVEN, C2, RIVERWOOD)]);
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: SVEN, origin: RIVERWOOD, dest: RIVERWOOD, adjacent: false, door: false, edge_units: u32::MAX }, 2_100), MoveVerdict::Accept);
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: SVEN, origin: RIVERWOOD, dest: RIVERWOOD, adjacent: false, door: false, edge_units: u32::MAX }, 2_200), MoveVerdict::Refuse);
        // a third player's claim is refused while c2 holds it; c2's own renews
        assert_eq!(c.claim(SVEN, C1, RIVERWOOD, Purpose::Talk, 2_300), ClaimVerdict::Held);
        assert_eq!(c.claim(SVEN, C2, RIVERWOOD, Purpose::Talk, 2_400), ClaimVerdict::Reclaimed);
        assert_eq!(c.owner_epoch_of(SVEN), 1, "a renewal changes no owner");
        // the release: the cell's host again
        assert_eq!(c.release_claim(SVEN, C1, Purpose::Talk), Release::NotHeld);
        assert_eq!(
            c.release_claim(SVEN, C2, Purpose::Talk),
            Release::Ended(Decision::ClaimEnded { actor: SVEN, cell: RIVERWOOD, player: C2, epoch: 2, why: ClaimEnd::Release })
        );
        assert_eq!(c.simulator_of(SVEN, RIVERWOOD), C1);
        // the lease: the tick ends a claim nobody released
        assert_eq!(c.claim(SVEN, C2, RIVERWOOD, Purpose::Talk, 3_000), ClaimVerdict::Granted);
        assert_eq!(c.tick(3_000 + CLAIM_LEASE_MS - 1), vec![]);
        assert_eq!(c.simulator_of(SVEN, RIVERWOOD), C2);
        assert_eq!(c.tick(3_000 + CLAIM_LEASE_MS), vec![Decision::ClaimEnded { actor: SVEN, cell: RIVERWOOD, player: C2, epoch: 4, why: ClaimEnd::Lease }]);
        assert_eq!(c.simulator_of(SVEN, RIVERWOOD), C1);
        // a claimant that has not loaded the cell gets nothing
        const EAST: u32 = 0x9733;
        assert_eq!(c.claim(SVEN, C2, EAST, Purpose::Talk, 4_000), ClaimVerdict::Unloaded);
        // a grant into the cell keeps a claim for its lease (ADR-052 as amended
        // 2026-10-28: playtest 36's regrant of 9732 to the claimant dropped its
        // claim on Sven and its release met E_CLAIM_NONE; to a third player the
        // actor would have flipped away mid-dialogue). Here c1 leaves and the
        // cell goes to c2, the claimant: the claim stands and its release finds it
        assert_eq!(c.claim(SVEN, C2, RIVERWOOD, Purpose::Talk, 5_000), ClaimVerdict::Granted);
        let _ = c.unloaded(C1, RIVERWOOD, 5_100);
        let regrant = c.tick(5_100 + GRACE_MS + 1);
        assert!(regrant.iter().any(|d| matches!(d, Decision::Grant { player: C2, cell: RIVERWOOD, .. })));
        assert_eq!(c.claims(), vec![(SVEN, C2, RIVERWOOD)]);
        assert_eq!(c.simulator_of(SVEN, RIVERWOOD), C2);
        assert!(matches!(c.release_claim(SVEN, C2, Purpose::Talk), Release::Ended(_)));
        assert_eq!(c.claims(), vec![]);
    }

    #[test]
    fn a_grant_to_another_player_keeps_a_dialogue_claim_until_its_lease() {
        // the third-player case: c2 holds Sven for a dialogue in c1's cell;
        // c1 leaves and the cell goes to c3 (the lower round trip; c2 stays
        // in the cell, since its own unload ends its claim, ADR-052 decision
        // 2); Sven stays c2's for the lease, the lease still ends it, and c3
        // simulates him then
        const SVEN: u32 = 0x13491;
        const C3: u32 = 0xff00_0002;
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C2, RIVERWOOD, 1_100), vec![]);
        assert_eq!(c.loaded(C3, RIVERWOOD, 1_200), vec![]);
        let _ = c.reach(C3, &[RIVERWOOD], 1_250);
        c.rtt(C2, 80);
        c.rtt(C3, 20);
        assert_eq!(c.claim(SVEN, C2, RIVERWOOD, Purpose::Talk, 2_000), ClaimVerdict::Granted);
        let _ = c.unloaded(C1, RIVERWOOD, 2_100);
        let regrant = c.tick(2_100 + GRACE_MS + 1);
        assert!(regrant.iter().any(|d| matches!(d, Decision::Grant { player: C3, cell: RIVERWOOD, .. })), "{regrant:?}");
        assert_eq!(c.claims(), vec![(SVEN, C2, RIVERWOOD)]);
        assert_eq!(c.simulator_of(SVEN, RIVERWOOD), C2);
        let ended = c.tick(2_000 + CLAIM_LEASE_MS);
        assert!(ended.iter().any(|d| matches!(d, Decision::ClaimEnded { actor: SVEN, cell: RIVERWOOD, why: ClaimEnd::Lease, .. })), "{ended:?}");
        assert_eq!(c.simulator_of(SVEN, RIVERWOOD), C3);
    }

    #[test]
    fn a_claim_travels_with_its_actor_across_cells() {
        // playtest 35 (2026-10-27): Hilde walked from 9732 into 9731, both the
        // other player's cells, under a guest's claim; the claim rides with
        // her for its lease, no follow, no pass, no word, and the release
        // still finds it
        const WEST: u32 = 0x9731;
        const HILDE: u32 = 0x35534;
        let mut c = both_in_reach();
        assert_eq!(c.loaded(C1, RIVERWOOD, 1_000), vec![Decision::Grant { player: C1, cell: RIVERWOOD, epoch: 1 }]);
        assert_eq!(c.loaded(C1, WEST, 1_050), vec![]);
        assert_eq!(c.reach(C1, &[RIVERWOOD, WEST], 1_100), vec![Decision::Grant { player: C1, cell: WEST, epoch: 1 }]);
        assert_eq!(c.loaded(C2, RIVERWOOD, 1_200), vec![]);
        assert_eq!(c.claim(HILDE, C2, RIVERWOOD, Purpose::Talk, 2_000), ClaimVerdict::Granted);
        assert_eq!(c.simulator_of(HILDE, RIVERWOOD), C2);
        // deep into the other host's next cell at once: no pass, the claim moves
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: HILDE, origin: RIVERWOOD, dest: WEST, adjacent: true, door: false, edge_units: 300 }, 2_500), MoveVerdict::Accept);
        assert_eq!(c.claims(), vec![(HILDE, C2, WEST)]);
        assert_eq!(c.simulator_of(HILDE, WEST), C2);
        assert_eq!(c.simulator_of(HILDE, RIVERWOOD), C1);
        // and back, near the edge: no follow row either, the claim moves again
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: HILDE, origin: WEST, dest: RIVERWOOD, adjacent: true, door: false, edge_units: 20 }, 2_600), MoveVerdict::Accept);
        assert_eq!(c.claims(), vec![(HILDE, C2, RIVERWOOD)]);
        // the host's own report of her is refused while the claim stands
        assert_eq!(c.may_move(&CellMove { sender: C1, actor: HILDE, origin: RIVERWOOD, dest: WEST, adjacent: true, door: false, edge_units: 300 }, 2_700), MoveVerdict::Refuse);
        // the release finds the claim after the crossings; the lease ends it in its current cell
        assert!(matches!(c.release_claim(HILDE, C2, Purpose::Talk), Release::Ended(_)));
        assert_eq!(c.simulator_of(HILDE, RIVERWOOD), C1);
        assert_eq!(c.claim(HILDE, C2, RIVERWOOD, Purpose::Talk, 3_000), ClaimVerdict::Granted);
        assert_eq!(c.may_move(&CellMove { sender: C2, actor: HILDE, origin: RIVERWOOD, dest: WEST, adjacent: true, door: false, edge_units: 300 }, 3_100), MoveVerdict::Accept);
        assert_eq!(c.tick(3_000 + CLAIM_LEASE_MS), vec![Decision::ClaimEnded { actor: HILDE, cell: WEST, player: C2, epoch: 4, why: ClaimEnd::Lease }]);
        assert_eq!(c.simulator_of(HILDE, WEST), C1);
        assert_eq!(c.claims(), vec![]);
    }

    // MS13 0x39645 and Riverwood Trader's people as Skyrim.esm and USSEP
    // hold them (records read on sky-srv, 2026-11-14): MS13's alias 9
    // "Camilla" fills by her unique actor (ALUA 0x1347b, her ACHR 0x13488 in
    // RiverwoodTrader 0x133c9, whose door gives on 0x9731); 0x38386's scene
    // 0x3839b, the argument, with Lucan 0x13487 its alias 0 and Camilla its
    // alias 2
    const MS13: u32 = 0x39645;
    const CAMILLA: u32 = 0x13488;
    const LUCAN: u32 = 0x13487;
    const TRADER: u32 = 0x133c9;
    const OUTSIDE: u32 = 0x9731;
    const ARGUMENT: u32 = 0x3839b;
    const ARGUMENT_QUEST: u32 = 0x38386;
    const CAMILLA_LEAD: Purpose = Purpose::Lead { quest: MS13, alias: 9 };

    #[test]
    fn a_lead_is_the_simulators_alone_and_wants_no_loaded_cell() {
        // D-53-4 and D-PO2 A ("the running game claims it"): the game running
        // a quest's own package on the quest's alias actor claims her; another
        // seat's lead is refused, and the simulator's wants no loaded report
        // (its engine may run her where its game has unloaded the cell)
        let mut c = both_in_reach();
        c.reach(C1, &[RIVERWOOD, TRADER], 0);
        c.reach(C2, &[RIVERWOOD, TRADER], 0);
        assert_eq!(c.loaded(C1, TRADER, 1_000), vec![Decision::Grant { player: C1, cell: TRADER, epoch: 1 }]);
        assert_eq!(c.loaded(C2, TRADER, 1_100), vec![]);
        assert_eq!(c.claim(CAMILLA, C2, TRADER, CAMILLA_LEAD, 2_000), ClaimVerdict::NotSimulator);
        assert_eq!(c.claims(), vec![]);
        assert_eq!(c.owner_epoch_of(CAMILLA), 0);
        assert_eq!(c.claim(CAMILLA, C1, TRADER, CAMILLA_LEAD, 2_100), ClaimVerdict::Granted);
        assert_eq!(
            c.claim_rows(),
            vec![ClaimRow { actor: CAMILLA, player: C1, cell: TRADER, talk: false, leads: vec![(MS13, 9)], scene: None, epoch: 1 }]
        );
        // the same quest's lead again: renewed, no new owner
        assert_eq!(c.claim(CAMILLA, C1, TRADER, CAMILLA_LEAD, 2_200), ClaimVerdict::Reclaimed);
        assert_eq!(c.owner_epoch_of(CAMILLA), 1);
        // no lease: the lead outlasts any talk's
        assert_eq!(c.tick(2_100 + 10 * CLAIM_LEASE_MS), vec![]);
        assert_eq!(c.simulator_of(CAMILLA, TRADER), C1);
        // the simulator of an actor it followed through a door into a cell its
        // game never loaded: the lead takes the follow's place; a talk there
        // wants the cell loaded, a lead's holder or not
        let mut d = both_in_reach();
        d.reach(C1, &[RIVERWOOD, TRADER], 0);
        d.loaded(C1, TRADER, 1_000);
        let door = CellMove { sender: C1, actor: CAMILLA, origin: TRADER, dest: OUTSIDE, adjacent: false, door: true, edge_units: u32::MAX };
        assert_eq!(d.may_move(&door, 2_000), MoveVerdict::Follow);
        assert_eq!(d.claim(CAMILLA, C1, OUTSIDE, CAMILLA_LEAD, 2_100), ClaimVerdict::Granted);
        assert_eq!(d.claims(), vec![(CAMILLA, C1, OUTSIDE)]);
        assert_eq!(d.claim(CAMILLA, C1, OUTSIDE, Purpose::Talk, 2_200), ClaimVerdict::Unloaded);
    }

    #[test]
    fn camillas_lead_goes_through_the_door_and_outlives_the_grant_of_the_cell_outside() {
        // R-C2 (playtest 52, 20:34:01Z to 20:34:53Z): rotfern's game hosted the
        // trader and walked Camilla out of its door on MS13's travel package
        // 0x2e4cc, a follow into 9731, which nobody hosted; ff000001 took 9731,
        // the grant dropped the follow, and every report of rotfern's game was
        // refused from then ("may not move 13488") while Ariadne's game ran
        // her own state. As a lead the row is a claim: it goes through the
        // door, the grant passes over it, the new host's report is refused and
        // the holder's accepted, the holder's unload of the outside keeps it,
        // and the quest's stop ends it
        let mut c = both_in_reach();
        c.reach(C1, &[RIVERWOOD, TRADER], 0);
        assert_eq!(c.loaded(C1, TRADER, 1_000), vec![Decision::Grant { player: C1, cell: TRADER, epoch: 1 }]);
        assert_eq!(c.claim(CAMILLA, C1, TRADER, CAMILLA_LEAD, 2_000), ClaimVerdict::Granted);
        let door = CellMove { sender: C1, actor: CAMILLA, origin: TRADER, dest: OUTSIDE, adjacent: false, door: true, edge_units: u32::MAX };
        assert_eq!(c.may_move(&door, 3_000), MoveVerdict::Accept);
        assert_eq!(c.claims(), vec![(CAMILLA, C1, OUTSIDE)]);
        assert_eq!(c.follows(), vec![(CAMILLA, C1, OUTSIDE)]);
        // c2 reaches and loads the outside: granted, the lead passes over it
        assert_eq!(c.reach(C2, &[RIVERWOOD, OUTSIDE], 3_500), vec![]);
        assert_eq!(c.loaded(C2, OUTSIDE, 4_000), vec![Decision::Grant { player: C2, cell: OUTSIDE, epoch: 1 }]);
        assert_eq!(c.claims(), vec![(CAMILLA, C1, OUTSIDE)]);
        assert_eq!(c.simulator_of(CAMILLA, OUTSIDE), C1);
        let at = |sender| CellMove { sender, actor: CAMILLA, origin: OUTSIDE, dest: OUTSIDE, adjacent: false, door: false, edge_units: 900 };
        assert_eq!(c.may_move(&at(C2), 4_100), MoveVerdict::Refuse);
        assert_eq!(c.may_move(&at(C1), 4_200), MoveVerdict::Accept);
        // the holder's game reporting the outside unloaded ends no lead, and
        // the new host's own departure passes over it too
        assert_eq!(c.unloaded(C1, OUTSIDE, 5_000), vec![]);
        assert_eq!(c.unloaded(C2, OUTSIDE, 6_000), vec![]);
        assert_eq!(c.tick(6_000 + GRACE_MS), vec![Decision::Release { player: C2, cell: OUTSIDE, epoch: 1 }, Decision::Unhosted { cell: OUTSIDE }]);
        assert_eq!(c.simulator_of(CAMILLA, OUTSIDE), C1);
        // MS13's stop ends it: the cell's rule again, nobody's there
        assert_eq!(c.quest_stopped(MS13), vec![Decision::ClaimEnded { actor: CAMILLA, cell: OUTSIDE, player: C1, epoch: 2, why: ClaimEnd::QuestStop }]);
        assert_eq!(c.simulator_of(CAMILLA, OUTSIDE), 0);
    }

    #[test]
    fn an_unload_ends_a_talk_and_keeps_a_lead() {
        // ADR-052 decision 2: the claimant leaving the cell ends its talk; a
        // lead stays (R-C2), and a row holding both loses the talk alone
        const SVEN: u32 = 0x13491;
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        c.loaded(C2, RIVERWOOD, 1_100);
        assert_eq!(c.claim(SVEN, C2, RIVERWOOD, Purpose::Talk, 2_000), ClaimVerdict::Granted);
        assert_eq!(c.unloaded(C2, RIVERWOOD, 3_000), vec![Decision::ClaimEnded { actor: SVEN, cell: RIVERWOOD, player: C2, epoch: 2, why: ClaimEnd::Unloaded }]);
        assert_eq!(c.simulator_of(SVEN, RIVERWOOD), C1);
        // the host's own talk beside its lead: the unload takes the talk
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, CAMILLA_LEAD, 4_000), ClaimVerdict::Granted);
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, Purpose::Talk, 4_100), ClaimVerdict::Granted);
        assert_eq!(c.owner_epoch_of(CAMILLA), 1, "a purpose joining its holder's row changes no owner");
        assert_eq!(c.unloaded(C1, RIVERWOOD, 5_000), vec![]);
        assert_eq!(
            c.claim_rows(),
            vec![ClaimRow { actor: CAMILLA, player: C1, cell: RIVERWOOD, talk: false, leads: vec![(MS13, 9)], scene: None, epoch: 1 }]
        );
    }

    #[test]
    fn each_end_of_a_claim_names_why_and_moves_the_owner_epoch() {
        // ADR-073 decision 6: each kind declares its own end events and
        // carries an ADR-066 epoch, up at the grant (1) and at the end (2)
        const SVEN: u32 = 0x13491;
        let setup = || {
            let mut c = both_in_reach();
            c.loaded(C1, RIVERWOOD, 1_000);
            c.loaded(C2, RIVERWOOD, 1_100);
            assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, CAMILLA_LEAD, 2_000), ClaimVerdict::Granted);
            c
        };
        let ended = |why| Decision::ClaimEnded { actor: CAMILLA, cell: RIVERWOOD, player: C1, epoch: 2, why };
        // the quest's stop; another quest's stops nothing
        let mut c = setup();
        assert_eq!(c.quest_stopped(ARGUMENT_QUEST), vec![]);
        assert_eq!(c.quest_stopped(MS13), vec![ended(ClaimEnd::QuestStop)]);
        assert_eq!(c.owner_epoch_of(CAMILLA), 2);
        // the alias cleared or given another: her own fill, or another alias's, keeps it
        let mut c = setup();
        assert_eq!(c.alias_filled(MS13, 9, CAMILLA), vec![]);
        assert_eq!(c.alias_filled(MS13, 2, 0), vec![]);
        assert_eq!(c.alias_filled(MS13, 9, LUCAN), vec![ended(ClaimEnd::Alias)]);
        // the actor's death
        let mut c = setup();
        assert_eq!(c.actor_died(LUCAN), vec![]);
        assert_eq!(c.actor_died(CAMILLA), vec![ended(ClaimEnd::Death)]);
        // the holder's departure
        let mut c = setup();
        assert_eq!(c.gone(C1, 3_000), vec![ended(ClaimEnd::Gone)]);
        assert_eq!(c.claims(), vec![]);
        // the holder's release; a release of what it does not hold, or by another, is none
        let mut c = setup();
        assert_eq!(c.release_claim(CAMILLA, C1, Purpose::Lead { quest: ARGUMENT_QUEST, alias: 9 }), Release::NotHeld);
        assert_eq!(c.release_claim(CAMILLA, C1, Purpose::Talk), Release::NotHeld);
        assert_eq!(c.release_claim(CAMILLA, C2, CAMILLA_LEAD), Release::NotHeld);
        assert_eq!(c.release_claim(CAMILLA, C1, Purpose::Lead { quest: MS13, alias: 0 }), Release::Ended(ended(ClaimEnd::Release)));
        // the holder's silence past the grace ends its lead, not its talk (the
        // talk keeps its lease); heard again inside the grace, it keeps both
        let mut c = setup();
        assert_eq!(c.claim(SVEN, C1, RIVERWOOD, Purpose::Talk, 2_500), ClaimVerdict::Granted);
        assert!(c.holds_lead_or_scene(C1));
        assert_eq!(c.stale(C1, 3_000), vec![]);
        assert_eq!(c.tick(3_000 + GRACE_MS - 1), vec![]);
        let out = c.tick(3_000 + GRACE_MS);
        assert!(out.contains(&ended(ClaimEnd::Stale)), "{out:?}");
        assert_eq!(c.claims(), vec![(SVEN, C1, RIVERWOOD)]);
        assert!(!c.holds_lead_or_scene(C1));
        let mut c = setup();
        assert_eq!(c.stale(C1, 3_000), vec![]);
        assert_eq!(c.revived(C1, 4_000), vec![]);
        assert_eq!(c.tick(3_000 + GRACE_MS), vec![]);
        assert_eq!(c.claims(), vec![(CAMILLA, C1, RIVERWOOD)]);
        // a dead holder keeps it: its engine still runs her, past the grace and the grant it gives
        let mut c = setup();
        assert_eq!(c.died(C1, 3_000), vec![]);
        let out = c.tick(3_000 + GRACE_MS);
        assert!(out.contains(&Decision::Grant { player: C2, cell: RIVERWOOD, epoch: 2 }), "{out:?}");
        assert!(!out.iter().any(|d| matches!(d, Decision::ClaimEnded { .. })), "{out:?}");
        assert_eq!(c.simulator_of(CAMILLA, RIVERWOOD), C1);
    }

    #[test]
    fn an_unresolved_lead_ends_at_no_aliass_refill_and_at_its_quests_stop() {
        // a lead the server cannot place on one of its quest's aliases
        // (alias_fills::ActorFill::Unresolved, R2) records UNRESOLVED_ALIAS,
        // never alias 0, a real alias id: neither alias 0's refill nor the
        // alias she really fills ends it; its quest's stop does
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        let unresolved = Purpose::Lead { quest: MS13, alias: UNRESOLVED_ALIAS };
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, unresolved, 2_000), ClaimVerdict::Granted);
        assert_eq!(c.alias_filled(MS13, 0, LUCAN), vec![]);
        assert_eq!(c.alias_filled(MS13, 9, LUCAN), vec![]);
        assert_eq!(c.claims(), vec![(CAMILLA, C1, RIVERWOOD)]);
        assert_eq!(c.quest_stopped(MS13), vec![Decision::ClaimEnded { actor: CAMILLA, cell: RIVERWOOD, player: C1, epoch: 2, why: ClaimEnd::QuestStop }]);
        assert_eq!(c.claims(), vec![]);
    }

    #[test]
    fn a_lead_ends_when_its_holders_game_unloads_her_and_she_returns_to_the_host() {
        // D-53-5 B: a lead also ends when its holder's game stops running the
        // actor, the holder's own report (R2), and she returns to her cell's
        // host. R-C2's shape: rotfern's game walks Camilla out of the trader's
        // door on her lead, Ariadne's is granted the outside, and rotfern's
        // game then holds her 3D no more
        let mut c = both_in_reach();
        c.reach(C1, &[RIVERWOOD, TRADER], 0);
        c.loaded(C1, TRADER, 1_000);
        assert_eq!(c.claim(CAMILLA, C1, TRADER, CAMILLA_LEAD, 2_000), ClaimVerdict::Granted);
        let door = CellMove { sender: C1, actor: CAMILLA, origin: TRADER, dest: OUTSIDE, adjacent: false, door: true, edge_units: u32::MAX };
        assert_eq!(c.may_move(&door, 3_000), MoveVerdict::Accept);
        c.reach(C2, &[RIVERWOOD, OUTSIDE], 3_500);
        assert_eq!(c.loaded(C2, OUTSIDE, 4_000), vec![Decision::Grant { player: C2, cell: OUTSIDE, epoch: 1 }]);
        assert_eq!(c.simulator_of(CAMILLA, OUTSIDE), C1);
        // another player's report, or one for a quest the row does not lead, is none
        assert_eq!(c.not_running(CAMILLA, C2, MS13, NotRunning::Unloaded), Release::NotHeld);
        assert_eq!(c.not_running(CAMILLA, C1, ARGUMENT_QUEST, NotRunning::Unloaded), Release::NotHeld);
        assert_eq!(c.claims(), vec![(CAMILLA, C1, OUTSIDE)]);
        // the holder's report ends it, its epoch up: the outside's host runs her
        assert_eq!(
            c.not_running(CAMILLA, C1, MS13, NotRunning::Unloaded),
            Release::Ended(Decision::ClaimEnded { actor: CAMILLA, cell: OUTSIDE, player: C1, epoch: 2, why: ClaimEnd::NotRunning(NotRunning::Unloaded) })
        );
        assert_eq!(c.claims(), vec![]);
        assert_eq!(c.simulator_of(CAMILLA, OUTSIDE), C2);
        let at = |sender| CellMove { sender, actor: CAMILLA, origin: OUTSIDE, dest: OUTSIDE, adjacent: false, door: false, edge_units: 900 };
        assert_eq!(c.may_move(&at(C2), 4_100), MoveVerdict::Accept);
        assert_eq!(c.may_move(&at(C1), 4_200), MoveVerdict::Refuse);
        // a second report finds nothing
        assert_eq!(c.not_running(CAMILLA, C1, MS13, NotRunning::Unloaded), Release::NotHeld);
    }

    #[test]
    fn a_package_of_another_quest_ends_that_quests_lead_alone() {
        // D-53-5 B: the package the holder's engine started on her is not one
        // of the lead's quest's own: that lead ends, the row standing for its
        // other purposes, and her return to the host waits on its last one
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        c.loaded(C2, RIVERWOOD, 1_100);
        assert_eq!(c.claim(CAMILLA, C2, RIVERWOOD, Purpose::Talk, 2_000), ClaimVerdict::Granted);
        assert_eq!(c.claim(CAMILLA, C2, RIVERWOOD, CAMILLA_LEAD, 2_100), ClaimVerdict::Granted);
        let argument_lead = Purpose::Lead { quest: ARGUMENT_QUEST, alias: 2 };
        assert_eq!(c.claim(CAMILLA, C2, RIVERWOOD, argument_lead, 2_200), ClaimVerdict::Granted);
        assert_eq!(c.not_running(CAMILLA, C2, MS13, NotRunning::PackageChanged), Release::Kept);
        assert_eq!(c.release_claim(CAMILLA, C2, Purpose::Talk), Release::Kept);
        assert_eq!(
            c.claim_rows(),
            vec![ClaimRow { actor: CAMILLA, player: C2, cell: RIVERWOOD, talk: false, leads: vec![(ARGUMENT_QUEST, 2)], scene: None, epoch: 1 }]
        );
        assert_eq!(c.simulator_of(CAMILLA, RIVERWOOD), C2);
        assert_eq!(
            c.not_running(CAMILLA, C2, ARGUMENT_QUEST, NotRunning::PackageChanged),
            Release::Ended(Decision::ClaimEnded {
                actor: CAMILLA,
                cell: RIVERWOOD,
                player: C2,
                epoch: 2,
                why: ClaimEnd::NotRunning(NotRunning::PackageChanged)
            })
        );
        assert_eq!(c.simulator_of(CAMILLA, RIVERWOOD), C1);
        assert_eq!(ClaimEnd::NotRunning(NotRunning::PackageChanged).name(), "its holder's game leaving the quest's own packages");
        assert_eq!(ClaimEnd::NotRunning(NotRunning::Unloaded).name(), "its holder's game unloading the actor");
    }

    #[test]
    fn the_lease_ends_a_talk_and_keeps_the_lead_beside_it() {
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, Purpose::Talk, 2_000), ClaimVerdict::Granted);
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, CAMILLA_LEAD, 2_100), ClaimVerdict::Granted);
        assert_eq!(c.tick(2_000 + CLAIM_LEASE_MS), vec![]);
        assert_eq!(
            c.claim_rows(),
            vec![ClaimRow { actor: CAMILLA, player: C1, cell: RIVERWOOD, talk: false, leads: vec![(MS13, 9)], scene: None, epoch: 1 }]
        );
        // the talk's release leaves the lead the same way
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, Purpose::Talk, 3_000), ClaimVerdict::Granted);
        assert_eq!(c.release_claim(CAMILLA, C1, Purpose::Talk), Release::Kept);
        assert_eq!(c.claims(), vec![(CAMILLA, C1, RIVERWOOD)]);
        assert_eq!(c.owner_epoch_of(CAMILLA), 1);
    }

    #[test]
    fn the_first_holder_keeps_the_actor_against_any_purpose_and_takes_its_own() {
        let mut c = both_in_reach();
        c.loaded(C1, RIVERWOOD, 1_000);
        c.loaded(C2, RIVERWOOD, 1_100);
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, CAMILLA_LEAD, 2_000), ClaimVerdict::Granted);
        // another seat, any purpose: held
        assert_eq!(c.claim(CAMILLA, C2, RIVERWOOD, Purpose::Talk, 2_100), ClaimVerdict::Held);
        assert_eq!(c.claim(CAMILLA, C2, RIVERWOOD, Purpose::Scene { scene: ARGUMENT, quest: ARGUMENT_QUEST }, 2_200), ClaimVerdict::Held);
        assert_eq!(c.claim(CAMILLA, C2, RIVERWOOD, Purpose::Lead { quest: ARGUMENT_QUEST, alias: 2 }, 2_300), ClaimVerdict::Held);
        // the holder's own talk joins its lead, and its release leaves the lead
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, Purpose::Talk, 2_400), ClaimVerdict::Granted);
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, Purpose::Talk, 2_500), ClaimVerdict::Reclaimed);
        assert_eq!(c.release_claim(CAMILLA, C1, Purpose::Talk), Release::Kept);
        assert_eq!(c.release_claim(CAMILLA, C1, Purpose::Talk), Release::NotHeld);
        assert_eq!(c.simulator_of(CAMILLA, RIVERWOOD), C1);
        // up to MAX_LEADS quests' leads; a fifth is refused and nothing held is dropped
        for quest in [ARGUMENT_QUEST, 0x5c625, 0x74a83] {
            assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, Purpose::Lead { quest, alias: 0 }, 3_000), ClaimVerdict::Granted);
        }
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, Purpose::Lead { quest: 0x25185, alias: 0 }, 3_100), ClaimVerdict::Full);
        assert_eq!(c.claim(CAMILLA, C1, RIVERWOOD, CAMILLA_LEAD, 3_200), ClaimVerdict::Reclaimed);
        assert_eq!(c.claim_rows().iter().map(|r| r.leads.len()).sum::<usize>(), MAX_LEADS);
        assert_eq!(c.owner_epoch_of(CAMILLA), 1);
    }

    #[test]
    fn a_scene_takes_its_actors_from_the_host_to_its_starter_until_it_ends() {
        // D-L2 B (the starting seat owns a scene, claiming its actors until it
        // ends) and D-A2 C (a fragment's scene is the stage's cause seat's; R2,
        // the first claim wins): c2 hosts the trader, c1 visits it and starts
        // the argument 0x3839b of 0x38386
        let mut c = both_in_reach();
        c.reach(C1, &[RIVERWOOD, TRADER], 0);
        c.reach(C2, &[RIVERWOOD, TRADER], 0);
        assert_eq!(c.loaded(C2, TRADER, 1_000), vec![Decision::Grant { player: C2, cell: TRADER, epoch: 1 }]);
        assert_eq!(c.loaded(C1, TRADER, 1_100), vec![]);
        let scene = Purpose::Scene { scene: ARGUMENT, quest: ARGUMENT_QUEST };
        assert_eq!(c.claim(LUCAN, C1, TRADER, scene, 2_000), ClaimVerdict::Granted);
        assert_eq!(c.claim(CAMILLA, C1, TRADER, scene, 2_000), ClaimVerdict::Granted);
        assert_eq!(c.simulator_of(LUCAN, TRADER), C1);
        assert!(c.holds_lead_or_scene(C1));
        assert!(!c.holds_lead_or_scene(C2));
        // the host's reports of them refused, the starter's accepted
        let at = |sender, actor| CellMove { sender, actor, origin: TRADER, dest: TRADER, adjacent: false, door: false, edge_units: u32::MAX };
        assert_eq!(c.may_move(&at(C2, LUCAN), 2_100), MoveVerdict::Refuse);
        assert_eq!(c.may_move(&at(C1, LUCAN), 2_200), MoveVerdict::Accept);
        // the host's own copy of the same scene claims nothing: the first claim wins
        assert_eq!(c.claim(LUCAN, C2, TRADER, scene, 2_300), ClaimVerdict::Held);
        // a seat that has not loaded the cell starts no scene in it
        const C3: u32 = 0xff00_0002;
        assert_eq!(c.claim(0x1348a, C3, TRADER, scene, 2_400), ClaimVerdict::Unloaded);
        // no lease
        assert_eq!(c.tick(2_000 + CLAIM_LEASE_MS), vec![]);
        // the quest's stop ends both, by actor id; the host again
        assert_eq!(
            c.quest_stopped(ARGUMENT_QUEST),
            vec![
                Decision::ClaimEnded { actor: LUCAN, cell: TRADER, player: C1, epoch: 2, why: ClaimEnd::QuestStop },
                Decision::ClaimEnded { actor: CAMILLA, cell: TRADER, player: C1, epoch: 2, why: ClaimEnd::QuestStop },
            ]
        );
        assert_eq!(c.simulator_of(LUCAN, TRADER), C2);
        assert!(!c.holds_lead_or_scene(C1));
        // the release at the scene's end returns its actor the same way (the
        // release names the scene; its quest is not read)
        assert_eq!(c.claim(LUCAN, C1, TRADER, scene, 3_000), ClaimVerdict::Granted);
        assert_eq!(c.release_claim(LUCAN, C1, Purpose::Scene { scene: 0x5dba7, quest: ARGUMENT_QUEST }), Release::NotHeld);
        assert_eq!(
            c.release_claim(LUCAN, C1, Purpose::Scene { scene: ARGUMENT, quest: 0 }),
            Release::Ended(Decision::ClaimEnded { actor: LUCAN, cell: TRADER, player: C1, epoch: 4, why: ClaimEnd::Release })
        );
        assert_eq!(c.simulator_of(LUCAN, TRADER), C2);
    }
}
