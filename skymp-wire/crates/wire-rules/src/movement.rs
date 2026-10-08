//! Movement speed bounds (thuum docs/verbs/movement-speed.md). How far a
//! player's actor may move over the ground: a budget that refills at the
//! fastest speed a player's movement types allow and holds a burst's worth,
//! so a dash, a knockback or a burst of updates after a stall passes while a
//! sustained speed above the game's does not. Height is free: falls are the
//! engine's. And the one jump the server permits (docs/verbs/console-commands.md,
//! COC): another cell, or farther than a move may go, once, where the server
//! named.

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

/// What a move a player's actor made means for its permitted jump.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JumpCheck {
    /// No jump is permitted: the move is judged as any other.
    NoPermit,
    /// The permitted jump: it passes, and the permit ends.
    Landed,
    /// A jump is permitted and this move is not it (a report from the
    /// loading screen, say): drop it without sending the player back.
    Waiting,
}

/// Every player actor's budget and permitted jump, by form id. Runtime
/// only: after a restart or a login a budget starts full, with no jump.
#[derive(Debug, Default)]
pub struct Budgets {
    by_actor: HashMap<u32, Budget>,
    jumps: HashMap<u32, (Landing, u64)>,
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

    /// Drop an actor's budget and permit (it left, or was destroyed).
    pub fn forget(&mut self, actor: u32) {
        self.by_actor.remove(&actor);
        self.jumps.remove(&actor);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn budgets_are_per_actor() {
        let mut all = Budgets::default();
        assert!(all.spend(1, BURST, 0));
        assert!(!all.spend(1, 1.0, 0));
        assert!(all.spend(2, BURST, 0));
        all.forget(1);
        assert!(all.spend(1, BURST, 0));
    }
}
