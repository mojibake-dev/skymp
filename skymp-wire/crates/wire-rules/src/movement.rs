//! Movement speed bounds (thuum docs/verbs/movement-speed.md). How far a
//! player's actor may move over the ground: a budget that refills at the
//! fastest speed a player's movement types allow and holds a burst's worth,
//! so a dash, a knockback or a burst of updates after a stall passes while a
//! sustained speed above the game's does not. Height is free: falls are the
//! engine's.

use std::collections::HashMap;

/// Horse and Vampire Lord sprint, 600 units a second, the fastest movement
/// type a player uses (Skyrim.esm Horse_Sprint_MT, Dawnguard.esm
/// VampireLordSprint_MT), plus a tenth.
pub const MAX_SPEED: f32 = 660.0;
/// The burst a budget holds.
pub const BURST: f32 = 2048.0;

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

/// Every player actor's budget, by form id. Runtime only: a fresh one after
/// a restart or a login starts full.
#[derive(Debug, Default)]
pub struct Budgets {
    by_actor: HashMap<u32, Budget>,
}

impl Budgets {
    /// Charge `actor` a move of `ground` units at `now_ms`.
    pub fn spend(&mut self, actor: u32, ground: f32, now_ms: u64) -> bool {
        self.by_actor.entry(actor).or_default().spend(ground, now_ms)
    }

    /// Drop an actor's budget (it left, or was destroyed).
    pub fn forget(&mut self, actor: u32) {
        self.by_actor.remove(&actor);
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
