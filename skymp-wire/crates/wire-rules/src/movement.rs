//! Movement speed bounds (thuum docs/verbs/movement-speed.md). How far a
//! player's actor may move over the ground: a budget that refills at the
//! fastest speed a player's movement types allow and holds a burst's worth,
//! so a dash, a knockback or a burst of updates after a stall passes while a
//! sustained speed above the game's does not. Height is free: falls are the
//! engine's. And the one jump the server permits (docs/verbs/console-commands.md,
//! COC): another cell, or farther than a move may go, once, where the server
//! named. And a cast teleport's (docs/verbs/cast-teleport.md): once, within
//! the spell's reach of where its caster stood at the cast. And the arrival
//! after a teleport the server made itself: the player's reports from
//! anywhere else were sent before the move.

use std::collections::HashMap;

/// Horse and Vampire Lord sprint, 600 units a second, the fastest movement
/// type a player uses (Skyrim.esm Horse_Sprint_MT, Dawnguard.esm
/// VampireLordSprint_MT), plus a tenth.
pub const MAX_SPEED: f32 = 660.0;
/// The burst a budget holds.
pub const BURST: f32 = 2048.0;
/// How long a permitted jump waits for the player's game to make it: a
/// loading screen, with room for a slow disk.
pub const JUMP_WAIT_MS: u64 = 60_000;
/// An exterior cell's side, units: the grid the server keeps references on
/// (libespm Browser.cpp files a reference under its position over 4096).
pub const CELL_SIDE: f32 = 4096.0;
/// How long the server waits, after it moved a player's actor itself (a
/// teleport: MoveTo, SetPos, `mp tp`, a respawn), for that player's game to
/// report from where it was put.
pub const ARRIVAL_WAIT_MS: u64 = 10_000;
/// How near the place the server put it a report must come from to count
/// as the arrival, units over the ground.
pub const ARRIVAL_RADIUS: f32 = 256.0;
/// The least a pin's radius may be: a standing player's own jitter, with room.
pub const PIN_RADIUS_MIN: f32 = 32.0;
/// How long a cast teleport's jump waits for its caster's game to report
/// the landing, unless its castTeleports entry names its own wait
/// (teleports.rs): the engine places the spell's marker, the mod's script
/// moves the player (Lost Grimoire's waits 0.12 s first), and the next
/// movement report carries it. HYPOTHESIS until x-teleport-probe measures
/// the time from the cast to the landing (docs/verbs/cast-teleport.md).
pub const CAST_JUMP_WAIT_MS: u64 = 5_000;
/// Slack on a cast jump's landing, units: the cast position's age and the
/// marker's placement on the ground, as a spell hit's (casts::RANGE_SLACK).
pub const CAST_JUMP_SLACK: f32 = 256.0;

/// One actor's budget.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Budget {
    left: f32,
    last_ms: Option<u64>,
}

impl Default for Budget {
    fn default() -> Self {
        Self { left: BURST, last_ms: None }
    }
}

impl Budget {
    /// True when a move of `ground` units fits at `now_ms` (any monotonic
    /// clock in milliseconds); only then is it spent. A distance that is not
    /// a finite, non-negative number never fits.
    pub fn spend(&mut self, ground: f32, now_ms: u64) -> bool {
        if let Some(last) = self.last_ms {
            let elapsed_ms = now_ms.saturating_sub(last);
            // u64 milliseconds to seconds; precision loss far beyond any
            // refill that matters (BURST / MAX_SPEED is about 3 s)
            let seconds = f32::from(u16::try_from(elapsed_ms.min(60_000)).unwrap_or(u16::MAX)) / 1000.0;
            self.left = (self.left + MAX_SPEED * seconds).min(BURST);
        }
        self.last_ms = Some(now_ms);
        if !(ground.is_finite() && ground >= 0.0) || ground > self.left {
            return false;
        }
        self.left -= ground;
        true
    }
}

/// Where a permitted jump may land.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Landing {
    /// Anywhere in an interior cell, by its form id.
    Interior(u32),
    /// An exterior cell, by its worldspace's form id and its grid square.
    /// The game picks the spot (a marker in the cell, else its middle; a
    /// HYPOTHESIS until the lab sees it), so within a square of the cell
    /// counts.
    Exterior {
        /// The worldspace.
        world: u32,
        /// The square's x.
        x: i16,
        /// The square's y.
        y: i16,
    },
}

impl Landing {
    /// Whether a position the game reported in `cell_or_world` is here.
    #[must_use]
    pub fn holds(self, cell_or_world: u32, x: f32, y: f32) -> bool {
        if !(x.is_finite() && y.is_finite()) {
            return false;
        }
        match self {
            Self::Interior(cell) => cell == cell_or_world,
            Self::Exterior { world, x: square_x, y: square_y } => {
                world == cell_or_world && near_square(x, square_x) && near_square(y, square_y)
            }
        }
    }
}

/// A coordinate within a square's span on one axis, give or take a square.
fn near_square(at: f32, square: i16) -> bool {
    let start = f32::from(square) * CELL_SIDE;
    at >= start - CELL_SIDE && at < start + 2.0 * CELL_SIDE
}

/// What a move a player's actor made means for its permitted jump, or for
/// the arrival the server expects after its own teleport.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JumpCheck {
    /// Nothing permitted or expected: the move is judged as any other.
    NoPermit,
    /// The permitted jump, or the arrival: it passes, and that ends.
    Landed,
    /// One waits and this move is not it (a report from the loading screen,
    /// or sent before the server's teleport): drop it without sending the
    /// player back.
    Waiting,
}

/// Where the server put a player's actor itself, which its game has not
/// reported from yet, and until when that is waited for.
#[derive(Debug, Clone, Copy, PartialEq)]
struct Arrival {
    cell_or_world: u32,
    x: f32,
    y: f32,
    until_ms: u64,
}

/// A place the server holds a player's actor at for a while after moving it
/// there: a report from farther than `ARRIVAL_RADIUS` before `until_ms` is
/// a move the server did not author (thuum docs/verbs/shadowbond.md: the
/// mod's own swap script in the caster's game moves the caster again after
/// the server's swap, to where the target's figure now stands, undoing it;
/// x-shadowbond 20261019-083537), and the player is sent back.
#[derive(Debug, Clone, Copy, PartialEq)]
struct Pin {
    cell_or_world: u32,
    x: f32,
    y: f32,
    /// How far a report may stray and still be the pinned place: the swap's
    /// own distance halved and clamped, since a swap shorter than the
    /// arrival radius (150 units in x-shadowbond 20261019-090907) slipped
    /// through a pin judged by `ARRIVAL_RADIUS`.
    radius: f32,
    until_ms: u64,
}

/// Where a cast teleport may land: within `reach` and the slack of where its
/// caster stood at the cast, in that cell or worldspace, until `until_ms`.
#[derive(Debug, Clone, Copy, PartialEq)]
struct CastJump {
    cell_or_world: u32,
    x: f32,
    y: f32,
    z: f32,
    reach: f32,
    until_ms: u64,
}

impl CastJump {
    /// Whether (x, y, z) in `cell_or_world` lies within the jump's reach and
    /// the slack of its point, in its cell or worldspace.
    fn covers(&self, cell_or_world: u32, x: f32, y: f32, z: f32) -> bool {
        let (dx, dy, dz) = (x - self.x, y - self.y, z - self.z);
        let distance = (dx * dx + dy * dy + dz * dz).sqrt();
        cell_or_world == self.cell_or_world && distance.is_finite() && distance <= self.reach + CAST_JUMP_SLACK
    }
}

/// Every player actor's budget and permitted jump, by form id. Runtime
/// only: after a restart or a login a budget starts full, with no jump.
#[derive(Debug, Default)]
pub struct Budgets {
    by_actor: HashMap<u32, Budget>,
    jumps: HashMap<u32, (Landing, u64)>,
    cast_jumps: HashMap<u32, CastJump>,
    arrivals: HashMap<u32, Arrival>,
    pins: HashMap<u32, Pin>,
}

impl Budgets {
    /// Charge `actor` a move of `ground` units at `now_ms`.
    pub fn spend(&mut self, actor: u32, ground: f32, now_ms: u64) -> bool {
        self.by_actor.entry(actor).or_default().spend(ground, now_ms)
    }

    /// Permit `actor` one jump to `to` until `JUMP_WAIT_MS` after `now_ms`;
    /// a later permit replaces it.
    pub fn permit_jump(&mut self, actor: u32, to: Landing, now_ms: u64) {
        self.jumps.insert(actor, (to, now_ms.saturating_add(JUMP_WAIT_MS)));
    }

    /// Judge a move of `actor`'s that the bounds refuse, to (x, y) in
    /// `cell_or_world` at `now_ms`, against its permit. A landing ends the
    /// permit, and so does the wait running out.
    pub fn check_jump(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) -> JumpCheck {
        let Some(&(to, until_ms)) = self.jumps.get(&actor) else {
            return JumpCheck::NoPermit;
        };
        if now_ms > until_ms {
            self.jumps.remove(&actor);
            return JumpCheck::NoPermit;
        }
        if !to.holds(cell_or_world, x, y) {
            return JumpCheck::Waiting;
        }
        self.jumps.remove(&actor);
        JumpCheck::Landed
    }

    /// `actor` cast a teleport at `now_ms`, standing at (x, y, z) in
    /// `cell_or_world`: one jump to within `reach` of there (and the slack;
    /// a reach of 0 is a return to where it cast) is permitted until
    /// `wait_ms` later; a later cast replaces it. A cast whose position or
    /// reach is not a finite number permits nothing.
    #[allow(clippy::too_many_arguments)]
    pub fn permit_cast_jump(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, reach: f32, wait_ms: u64, now_ms: u64) {
        if !(x.is_finite() && y.is_finite() && z.is_finite() && reach.is_finite()) {
            self.cast_jumps.remove(&actor);
            return;
        }
        let until_ms = now_ms.saturating_add(wait_ms);
        self.cast_jumps.insert(actor, CastJump { cell_or_world, x, y, z, reach: reach.max(0.0), until_ms });
    }

    /// Judge a move of `actor`'s that the bounds refuse, to (x, y, z) in
    /// `cell_or_world` at `now_ms`, against its cast jump: Landed within the
    /// reach and slack of the cast, in its cell or worldspace (that ends the
    /// jump); NoPermit otherwise, so the move is judged as any other (no
    /// loading screen to wait through, unlike COC). The wait running out
    /// ends the jump.
    pub fn check_cast_jump(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, now_ms: u64) -> JumpCheck {
        let Some(&j) = self.cast_jumps.get(&actor) else {
            return JumpCheck::NoPermit;
        };
        if now_ms > j.until_ms {
            self.cast_jumps.remove(&actor);
            return JumpCheck::NoPermit;
        }
        if !j.covers(cell_or_world, x, y, z) {
            return JumpCheck::NoPermit;
        }
        self.cast_jumps.remove(&actor);
        JumpCheck::Landed
    }

    /// Whether a move of `actor`'s that the bounds took, to (x, y, z) in
    /// `cell_or_world` at `now_ms`, lands within the cast jump that waits
    /// for it: the other games place its figure there instead of gliding it
    /// (thuum docs/verbs/cast-teleport.md, the teleport snap;
    /// docs/verbs/shadowbond.md, the swap's two landings, each within the
    /// ground budget's burst). The jump stands: it is the permit for a move
    /// the bounds refuse, which a move they took never needed. The caller
    /// asks for a step no run makes between two reports alone
    /// (teleports::look_lands): a short jump's point lies within reach of
    /// where the actor already stands, so any nearer report would land too.
    #[must_use]
    pub fn cast_jump_lands(&self, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, now_ms: u64) -> bool {
        self.cast_jumps.get(&actor).is_some_and(|j| now_ms <= j.until_ms && j.covers(cell_or_world, x, y, z))
    }

    /// The server moved `actor` itself, to (x, y) in `cell_or_world`, at
    /// `now_ms`: until its game reports from there, or `ARRIVAL_WAIT_MS`
    /// passes, its reports from anywhere else were sent before the move. A
    /// later teleport replaces it.
    pub fn expect_arrival(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) {
        self.arrivals.insert(actor, Arrival { cell_or_world, x, y, until_ms: now_ms.saturating_add(ARRIVAL_WAIT_MS) });
    }

    /// Hold `actor` at (x, y) in `cell_or_world` until `until_ms`: a report
    /// from farther than `radius` before then is a move the server did not
    /// author, and `pinned_away` says so (the caller sends the player back).
    /// The radius is clamped to [`PIN_RADIUS_MIN`, `ARRIVAL_RADIUS`]. The
    /// server's own next move of the actor replaces the pin.
    pub fn pin(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, radius: f32, until_ms: u64) {
        let radius = if radius.is_finite() { radius.clamp(PIN_RADIUS_MIN, ARRIVAL_RADIUS) } else { PIN_RADIUS_MIN };
        self.pins.insert(actor, Pin { cell_or_world, x, y, radius, until_ms });
    }

    /// Whether a report of `actor`'s from (x, y) in `cell_or_world` at
    /// `now_ms` leaves the place it is pinned to while the pin holds; a pin
    /// that ran out is forgotten.
    pub fn pinned_away(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) -> bool {
        let Some(&p) = self.pins.get(&actor) else {
            return false;
        };
        if now_ms > p.until_ms {
            self.pins.remove(&actor);
            return false;
        }
        let near = x.is_finite() && y.is_finite() && (x - p.x).hypot(y - p.y) <= p.radius;
        cell_or_world != p.cell_or_world || !near
    }

    /// Judge any report of `actor`'s, from (x, y) in `cell_or_world` at
    /// `now_ms`, against the arrival the server expects: Landed from within
    /// `ARRIVAL_RADIUS` of it (that ends it), Waiting from elsewhere (drop
    /// the report), NoPermit when none is expected or the wait ran out.
    pub fn check_arrival(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) -> JumpCheck {
        let Some(&a) = self.arrivals.get(&actor) else {
            return JumpCheck::NoPermit;
        };
        if now_ms > a.until_ms {
            self.arrivals.remove(&actor);
            return JumpCheck::NoPermit;
        }
        let near = x.is_finite() && y.is_finite() && (x - a.x).hypot(y - a.y) <= ARRIVAL_RADIUS;
        if cell_or_world != a.cell_or_world || !near {
            return JumpCheck::Waiting;
        }
        self.arrivals.remove(&actor);
        JumpCheck::Landed
    }

    /// Drop an actor's budget, permit and expected arrival (it left, or was
    /// destroyed).
    pub fn forget(&mut self, actor: u32) {
        self.by_actor.remove(&actor);
        self.jumps.remove(&actor);
        self.cast_jumps.remove(&actor);
        self.arrivals.remove(&actor);
        self.pins.remove(&actor);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // thuum docs/verbs/cast-teleport.md: Lost Grimoire's Teleportation cast
    // from (0, -300, 12) on lab-spawn's mountain (the Tamriel worldspace,
    // 0x3C) with a reach of 3000
    const TAMRIEL_ID: u32 = 0x3c;

    #[test]
    fn a_cast_jump_lands_once_within_reach_of_the_cast() {
        let mut all = Budgets::default();
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 2_000.0, 12.0, 0), JumpCheck::NoPermit);
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, -300.0, 12.0, 3_000.0, CAST_JUMP_WAIT_MS, 1_000);
        // another actor's jump is its own
        assert_eq!(all.check_cast_jump(2, TAMRIEL_ID, 0.0, 2_000.0, 12.0, 1_200), JumpCheck::NoPermit);
        // 2300 units off and 400 down: within 3000 and the slack
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 2_000.0, -388.0, 1_500), JumpCheck::Landed);
        // once
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 2_000.0, -388.0, 1_600), JumpCheck::NoPermit);
    }

    #[test]
    fn a_cast_jump_beyond_reach_elsewhere_or_late_is_any_other_move() {
        let mut all = Budgets::default();
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, -300.0, 12.0, 3_000.0, CAST_JUMP_WAIT_MS, 0);
        // beyond 3000 and the slack: judged as any move, the jump still waits
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 3_000.0, 12.0, 100), JumpCheck::NoPermit);
        // another cell or worldspace
        assert_eq!(all.check_cast_jump(1, 0x1_3a8f, 0.0, 0.0, 0.0, 200), JumpCheck::NoPermit);
        // within reach, now: lands
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 2_900.0, 12.0, 300), JumpCheck::Landed);
        // the wait runs out
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, -300.0, 12.0, 3_000.0, CAST_JUMP_WAIT_MS, 0);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 0.0, 12.0, CAST_JUMP_WAIT_MS.saturating_add(1)), JumpCheck::NoPermit);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 0.0, 12.0, 1), JumpCheck::NoPermit);
    }

    #[test]
    fn a_step_the_budget_takes_lands_within_the_waiting_cast_jump_and_leaves_it() {
        // thuum docs/verbs/shadowbond.md: the swap's landing, 300 units off
        // (within the budget's burst), at the other actor's place, which the
        // bond permits within its slack for its window
        let mut all = Budgets::default();
        assert!(!all.cast_jump_lands(1, TAMRIEL_ID, 0.0, 300.0, 0.0, 0));
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, 300.0, 0.0, 256.0, 1_500, 1_000);
        // another actor's, another worldspace, beyond the slack, no number
        assert!(!all.cast_jump_lands(2, TAMRIEL_ID, 0.0, 300.0, 0.0, 1_100));
        assert!(!all.cast_jump_lands(1, 0x1_3a8f, 0.0, 300.0, 0.0, 1_100));
        assert!(!all.cast_jump_lands(1, TAMRIEL_ID, 0.0, 900.0, 0.0, 1_100));
        assert!(!all.cast_jump_lands(1, TAMRIEL_ID, f32::NAN, 300.0, 0.0, 1_100));
        // at the other's place: lands, and the jump stands for a move the
        // bounds refuse
        assert!(all.cast_jump_lands(1, TAMRIEL_ID, 0.0, 300.0, 0.0, 1_100));
        assert!(all.cast_jump_lands(1, TAMRIEL_ID, 100.0, 400.0, 0.0, 1_200));
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 300.0, 0.0, 1_300), JumpCheck::Landed);
        assert!(!all.cast_jump_lands(1, TAMRIEL_ID, 0.0, 300.0, 0.0, 1_400));
        // the window runs out
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, 300.0, 0.0, 256.0, 1_500, 2_000);
        assert!(all.cast_jump_lands(1, TAMRIEL_ID, 0.0, 300.0, 0.0, 3_500));
        assert!(!all.cast_jump_lands(1, TAMRIEL_ID, 0.0, 300.0, 0.0, 3_501));
    }

    #[test]
    fn a_return_lands_once_back_where_it_was_cast_within_its_own_wait() {
        // Ghostwalk's entry (thuum docs/verbs/ghostwalk.md): reach 0, 15.4 s
        let mut all = Budgets::default();
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, -300.0, 12.0, 0.0, 15_400, 1_000);
        // walked 3000 units off: not the return, judged as any move
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 2_700.0, 12.0, 6_000), JumpCheck::NoPermit);
        // past the default wait, within its own, back within the slack
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 100.0, -300.0, 12.0, 11_400), JumpCheck::Landed);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 100.0, -300.0, 12.0, 11_500), JumpCheck::NoPermit);
        // 300 units from the cast point is past the slack
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, -300.0, 12.0, 0.0, 15_400, 20_000);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 300.0, -300.0, 12.0, 30_000), JumpCheck::NoPermit);
        // and its wait runs out
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, -300.0, 12.0, 35_401), JumpCheck::NoPermit);
    }

    #[test]
    fn a_later_cast_replaces_the_jump_and_nonsense_permits_nothing() {
        let mut all = Budgets::default();
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, -300.0, 12.0, 3_000.0, CAST_JUMP_WAIT_MS, 0);
        all.permit_cast_jump(1, TAMRIEL_ID, 10_000.0, 10_000.0, 0.0, 500.0, CAST_JUMP_WAIT_MS, 10);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 2_000.0, 12.0, 20), JumpCheck::NoPermit);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 10_400.0, 10_000.0, 0.0, 30), JumpCheck::Landed);
        all.permit_cast_jump(1, TAMRIEL_ID, f32::NAN, 0.0, 0.0, 3_000.0, CAST_JUMP_WAIT_MS, 40);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 0.0, 0.0, 50), JumpCheck::NoPermit);
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, 0.0, 0.0, f32::INFINITY, CAST_JUMP_WAIT_MS, 60);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 0.0, 0.0, 70), JumpCheck::NoPermit);
        all.permit_cast_jump(1, TAMRIEL_ID, 0.0, 0.0, 0.0, 3_000.0, CAST_JUMP_WAIT_MS, 80);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, f32::NAN, 0.0, 0.0, 90), JumpCheck::NoPermit);
        all.forget(1);
        assert_eq!(all.check_cast_jump(1, TAMRIEL_ID, 0.0, 0.0, 0.0, 100), JumpCheck::NoPermit);
    }

    #[test]
    fn the_budget_holds_a_burst_and_refills_at_the_top_speed() {
        let mut b = Budget::default();
        assert!(b.spend(BURST, 1_000));
        assert!(!b.spend(1.0, 1_000));
        // a refused move spends nothing; a second later a second's worth is back
        assert!(b.spend(MAX_SPEED, 2_000));
        assert!(!b.spend(1.0, 2_000));
        // an idle minute refills no more than the burst
        assert!(b.spend(BURST, 62_000));
        assert!(!b.spend(1.0, 62_000));
    }

    #[test]
    fn a_horses_sprint_fits_and_a_speed_hack_does_not() {
        // one update every 130 ms (skymp5-client sendInputsService.ts)
        let (mut rider, mut hacker) = (Budget::default(), Budget::default());
        let mut refused_at = None;
        let mut rider_fits = true;
        for i in 0..600u64 {
            let t = i.saturating_mul(130);
            rider_fits &= rider.spend(600.0 * 0.13, t);
            if !hacker.spend(1500.0 * 0.13, t) && refused_at.is_none() {
                refused_at = Some(i);
            }
        }
        assert!(rider_fits);
        assert!(matches!(refused_at, Some(i) if i < 20), "{refused_at:?}");
    }

    #[test]
    fn nonsense_distances_never_fit_and_a_clock_going_back_refills_nothing() {
        let mut b = Budget::default();
        assert!(!b.spend(f32::NAN, 0));
        assert!(!b.spend(-1.0, 0));
        assert!(!b.spend(f32::INFINITY, 0));
        assert!(b.spend(BURST, 10_000));
        assert!(!b.spend(1.0, 5_000));
    }

    const TAMRIEL: u32 = 0x3c;
    // Skyrim.esm: Riverwood is Tamriel's square (4, -12), its inn the
    // interior cell 0x133c6 (thuum lab/esm.py, 2026-10-08)
    const RIVERWOOD: Landing = Landing::Exterior { world: TAMRIEL, x: 4, y: -12 };
    const INN: u32 = 0x133c6;

    #[test]
    fn a_permitted_jump_lands_once_in_its_cell() {
        let mut all = Budgets::default();
        assert_eq!(all.check_jump(1, INN, 0.0, 0.0, 0), JumpCheck::NoPermit);
        all.permit_jump(1, Landing::Interior(INN), 1_000);
        // another actor has none; another cell, or none, is not the landing
        assert_eq!(all.check_jump(2, INN, 0.0, 0.0, 1_500), JumpCheck::NoPermit);
        assert_eq!(all.check_jump(1, 0x133c7, 0.0, 0.0, 1_500), JumpCheck::Waiting);
        assert_eq!(all.check_jump(1, 0, 0.0, 0.0, 1_500), JumpCheck::Waiting);
        assert_eq!(all.check_jump(1, INN, f32::NAN, 0.0, 1_500), JumpCheck::Waiting);
        assert_eq!(all.check_jump(1, INN, -300.0, 2_000.0, 2_000), JumpCheck::Landed);
        assert_eq!(all.check_jump(1, INN, 0.0, 0.0, 2_100), JumpCheck::NoPermit);
    }

    #[test]
    fn an_exterior_landing_is_within_a_square_of_its_cell() {
        let (mid_x, mid_y) = (4.5 * CELL_SIDE, -11.5 * CELL_SIDE);
        assert!(RIVERWOOD.holds(TAMRIEL, mid_x, mid_y));
        // a square off either way still counts; past that it does not
        assert!(RIVERWOOD.holds(TAMRIEL, 3.0 * CELL_SIDE, -13.0 * CELL_SIDE));
        assert!(RIVERWOOD.holds(TAMRIEL, 5.9 * CELL_SIDE, -10.1 * CELL_SIDE));
        assert!(!RIVERWOOD.holds(TAMRIEL, 6.0 * CELL_SIDE, mid_y));
        assert!(!RIVERWOOD.holds(TAMRIEL, mid_x, -13.1 * CELL_SIDE));
        // another worldspace, or a position that is no number, never
        assert!(!RIVERWOOD.holds(0x3d, mid_x, mid_y));
        assert!(!RIVERWOOD.holds(TAMRIEL, f32::NAN, mid_y));
        assert!(!RIVERWOOD.holds(TAMRIEL, mid_x, f32::INFINITY));
        let mut all = Budgets::default();
        all.permit_jump(1, RIVERWOOD, 0);
        assert_eq!(all.check_jump(1, TAMRIEL, 0.0, 0.0, 10), JumpCheck::Waiting);
        assert_eq!(all.check_jump(1, TAMRIEL, mid_x, mid_y, 20), JumpCheck::Landed);
    }

    #[test]
    fn a_permit_waits_a_minute_and_the_last_one_counts() {
        let mut all = Budgets::default();
        all.permit_jump(1, Landing::Interior(INN), 0);
        assert_eq!(all.check_jump(1, INN, 0.0, 0.0, JUMP_WAIT_MS.saturating_add(1)), JumpCheck::NoPermit);
        assert_eq!(all.check_jump(1, INN, 0.0, 0.0, 2), JumpCheck::NoPermit);
        all.permit_jump(1, Landing::Interior(INN), 0);
        all.permit_jump(1, RIVERWOOD, 0);
        assert_eq!(all.check_jump(1, INN, 0.0, 0.0, JUMP_WAIT_MS), JumpCheck::Waiting);
        all.forget(1);
        assert_eq!(all.check_jump(1, TAMRIEL, 4.5 * CELL_SIDE, -11.5 * CELL_SIDE, 1), JumpCheck::NoPermit);
    }

    #[test]
    fn a_teleport_the_server_made_waits_for_its_arrival() {
        let mut all = Budgets::default();
        assert_eq!(all.check_arrival(1, TAMRIEL, 0.0, 0.0, 0), JumpCheck::NoPermit);
        all.expect_arrival(1, TAMRIEL, 1_000.0, 2_000.0, 0);
        // the reports sent before the move, from the old place, nearby or
        // not, and from the loading screen (no cell), are dropped
        assert_eq!(all.check_arrival(1, TAMRIEL, 300.0, 2_000.0, 10), JumpCheck::Waiting);
        assert_eq!(all.check_arrival(1, TAMRIEL, 1_300.0, 2_000.0, 20), JumpCheck::Waiting);
        assert_eq!(all.check_arrival(1, 0, 1_000.0, 2_000.0, 30), JumpCheck::Waiting);
        assert_eq!(all.check_arrival(1, TAMRIEL, f32::NAN, 2_000.0, 40), JumpCheck::Waiting);
        // another actor expects nothing
        assert_eq!(all.check_arrival(2, TAMRIEL, 300.0, 2_000.0, 50), JumpCheck::NoPermit);
        // the arrival, within the radius, once
        assert_eq!(all.check_arrival(1, TAMRIEL, 1_100.0, 2_150.0, 60), JumpCheck::Landed);
        assert_eq!(all.check_arrival(1, TAMRIEL, 300.0, 2_000.0, 70), JumpCheck::NoPermit);
    }

    #[test]
    fn an_arrival_waits_ten_seconds_and_the_last_teleport_counts() {
        let mut all = Budgets::default();
        all.expect_arrival(1, INN, 0.0, 0.0, 0);
        assert_eq!(all.check_arrival(1, INN, 900.0, 0.0, ARRIVAL_WAIT_MS.saturating_add(1)), JumpCheck::NoPermit);
        all.expect_arrival(1, INN, 0.0, 0.0, 0);
        all.expect_arrival(1, TAMRIEL, 5_000.0, 5_000.0, 0);
        assert_eq!(all.check_arrival(1, INN, 0.0, 0.0, 1), JumpCheck::Waiting);
        assert_eq!(all.check_arrival(1, TAMRIEL, 5_000.0, 5_000.0, 2), JumpCheck::Landed);
        all.expect_arrival(1, INN, 0.0, 0.0, 0);
        all.forget(1);
        assert_eq!(all.check_arrival(1, TAMRIEL, 9.0, 9.0, 1), JumpCheck::NoPermit);
    }

    #[test]
    fn a_pin_sends_back_a_move_the_server_did_not_author_while_it_holds() {
        // thuum docs/verbs/shadowbond.md: after the server's swap the mod's
        // script in the caster's game moves the caster again, 150 units to
        // where the target's figure now stands (x-shadowbond 20261019-083537)
        let mut all = Budgets::default();
        assert!(!all.pinned_away(1, TAMRIEL, 0.0, 300.0, 0));
        all.pin(1, TAMRIEL, 0.0, 300.0, 150.0, 1_500);
        // within the radius: not away; the script's move back: away
        assert!(!all.pinned_away(1, TAMRIEL, 10.0, 290.0, 100));
        assert!(all.pinned_away(1, TAMRIEL, 0.0, 0.0, 250));
        assert!(all.pinned_away(1, 0x1_3a8f, 0.0, 300.0, 300));
        assert!(all.pinned_away(1, TAMRIEL, f32::NAN, 300.0, 350));
        // another actor is not pinned; the pin runs out; a forgotten actor's goes
        assert!(!all.pinned_away(2, TAMRIEL, 0.0, 0.0, 400));
        assert!(!all.pinned_away(1, TAMRIEL, 0.0, 0.0, 1_501));
        all.pin(1, TAMRIEL, 0.0, 300.0, 150.0, 5_000);
        all.forget(1);
        assert!(!all.pinned_away(1, TAMRIEL, 0.0, 0.0, 2_000));
        // a swap of 150 units (x-shadowbond 20261019-090907, the two 150 apart):
        // the radius is the half, 75, so the script's 150-unit move is away
        // while a jitter of 20 is not; the clamp holds the ends
        all.pin(1, TAMRIEL, 0.0, 150.0, 75.0, 1_500);
        assert!(all.pinned_away(1, TAMRIEL, 0.0, 0.0, 300));
        assert!(!all.pinned_away(1, TAMRIEL, 0.0, 130.0, 300));
        all.pin(1, TAMRIEL, 0.0, 0.0, 5.0, 1_500);
        assert!(!all.pinned_away(1, TAMRIEL, 0.0, 31.0, 300));
        assert!(all.pinned_away(1, TAMRIEL, 0.0, 33.0, 300));
        all.pin(1, TAMRIEL, 0.0, 0.0, 10_000.0, 1_500);
        assert!(!all.pinned_away(1, TAMRIEL, 0.0, 255.0, 300));
        assert!(all.pinned_away(1, TAMRIEL, 0.0, 257.0, 300));
    }

    #[test]
    fn budgets_are_per_actor() {
        let mut all = Budgets::default();
        assert!(all.spend(1, BURST, 0));
        assert!(!all.spend(1, 1.0, 0));
        assert!(all.spend(2, BURST, 0));
        all.forget(1);
        assert!(all.spend(1, BURST, 0));
    }
}
