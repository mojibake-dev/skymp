//! Rest (thuum docs/verbs/rest.md, ADR-021 decision 2). A player waits or
//! sleeps for themselves: the shared clock does not move, and the server gives
//! the player what the rest would give. The client reports the hours (R1); the
//! server checks them, that the player is alive and out of a fight, and
//! computes the recovery (R0) at the regeneration rates it already crops
//! reports with (CropRegeneration).
//!
//! Engine facts (UESP, Skyrim:Health): health regenerates 0.70 percent of its
//! maximum a second out of combat; a rest restores it fully under normal
//! circumstances, 142.86 s of regeneration being "well under the hour minimum
//! of waiting or sleeping".

/// The wait menu's range in game hours: an hour at the least, a day at most.
pub const MIN_HOURS: f32 = 1.0;
/// See [`MIN_HOURS`].
pub const MAX_HOURS: f32 = 24.0;

/// No rest within this long of a hit the player dealt or took, as the server
/// saw it: the server's stand-in for the engine's "not with enemies nearby",
/// which only the client can see. A choice, not an engine number.
pub const COMBAT_QUIET_MS: u64 = 10_000;

/// Game seconds in a game hour. HYPOTHESIS: the engine regenerates over a
/// rest's game seconds rather than their real-time equivalent at the time
/// scale; both restore fully at the base rates, and rest.md's Dynamic plan
/// measures which one with a lowered rate.
pub const REGEN_SECONDS_PER_HOUR: f32 = 3_600.0;

/// What the server knows about a player's rest.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RestFacts {
    /// The hours the client says its player rested.
    pub hours: f32,
    /// The server holds the player dead.
    pub is_dead: bool,
    /// Milliseconds since the last hit the player dealt or took, if any.
    pub since_last_hit_ms: Option<u64>,
}

/// Why a rest is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// Hours outside the menu's range, or not a number.
    Hours,
    /// The player is dead.
    Dead,
    /// A hit within [`COMBAT_QUIET_MS`].
    Fighting,
}

/// Whether the server lets a rest through, and why not when it does not.
pub fn check(f: RestFacts) -> Result<(), Refusal> {
    if !(MIN_HOURS..=MAX_HOURS).contains(&f.hours) {
        return Err(Refusal::Hours);
    }
    if f.is_dead {
        return Err(Refusal::Dead);
    }
    if matches!(f.since_last_hit_ms, Some(ms) if ms < COMBAT_QUIET_MS) {
        return Err(Refusal::Fighting);
    }
    Ok(())
}

/// One attribute as CropRegeneration takes it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Regen {
    /// The current percentage, 0 to 1.
    pub percentage: f32,
    /// The regeneration rate, percent of the maximum a second.
    pub rate: f32,
    /// The rate multiplier, percent.
    pub rate_mult: f32,
}

/// The attribute's percentage after a rest of `hours`: what it regenerates
/// over the rest at its rate, capped at full. A rate that is not a number, or
/// negative, regenerates nothing; a rest never lowers an attribute.
pub fn after_rest(r: Regen, hours: f32) -> f32 {
    let start = if r.percentage.is_finite() { r.percentage.clamp(0.0, 1.0) } else { 0.0 };
    let gained = (r.rate / 100.0) * (r.rate_mult / 100.0) * hours * REGEN_SECONDS_PER_HOUR;
    if !gained.is_finite() || gained <= 0.0 {
        return start;
    }
    (start + gained).min(1.0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    const fn facts(hours: f32, dead: bool, since: Option<u64>) -> RestFacts {
        RestFacts { hours, is_dead: dead, since_last_hit_ms: since }
    }

    #[test]
    fn the_menu_range_alive_and_quiet() {
        assert_eq!(check(facts(1.0, false, None)), Ok(()));
        assert_eq!(check(facts(24.0, false, Some(10_000))), Ok(()));
        assert_eq!(check(facts(0.5, false, None)), Err(Refusal::Hours));
        assert_eq!(check(facts(24.01, false, None)), Err(Refusal::Hours));
        assert_eq!(check(facts(f32::NAN, false, None)), Err(Refusal::Hours));
        assert_eq!(check(facts(f32::INFINITY, false, None)), Err(Refusal::Hours));
        assert_eq!(check(facts(2.0, true, None)), Err(Refusal::Dead));
        assert_eq!(check(facts(2.0, false, Some(9_999))), Err(Refusal::Fighting));
    }

    #[test]
    fn an_hour_restores_health_fully() {
        // UESP's out-of-combat health rate, 0.70 percent a second, at a 100
        // percent multiplier: half health is full after the menu's hour
        let health = Regen { percentage: 0.5, rate: 0.7, rate_mult: 100.0 };
        assert!((after_rest(health, 1.0) - 1.0).abs() < f32::EPSILON);
    }

    #[test]
    fn the_dynamic_plan_s_lowered_rate() {
        // rest.md: HealRateMult 1 percent, an hour from half health
        let lowered = Regen { percentage: 0.5, rate: 0.7, rate_mult: 1.0 };
        assert!((after_rest(lowered, 1.0) - 0.752).abs() < 1e-5);
    }

    #[test]
    fn nothing_regenerates_without_a_rate() {
        for (rate, mult) in [(0.0, 100.0), (0.7, 0.0), (-1.0, 100.0), (f32::NAN, 100.0), (0.7, f32::INFINITY)] {
            let r = Regen { percentage: 0.25, rate, rate_mult: mult };
            assert!((after_rest(r, 8.0) - 0.25).abs() < f32::EPSILON, "{rate} {mult}");
        }
    }

    proptest! {
        #[test]
        fn a_rest_never_lowers_and_never_passes_full(p in -1.0f32..2.0, rate in -10.0f32..10.0, mult in -100.0f32..300.0, hours in MIN_HOURS..=MAX_HOURS) {
            let after = after_rest(Regen { percentage: p, rate, rate_mult: mult }, hours);
            prop_assert!((0.0..=1.0).contains(&after));
            prop_assert!(after >= p.clamp(0.0, 1.0));
        }

        #[test]
        fn longer_rests_restore_at_least_as_much(p in 0.0f32..1.0, rate in 0.0f32..10.0, mult in 0.0f32..200.0, a in MIN_HOURS..=MAX_HOURS, b in MIN_HOURS..=MAX_HOURS) {
            let (short, long) = if a <= b { (a, b) } else { (b, a) };
            let r = Regen { percentage: p, rate, rate_mult: mult };
            prop_assert!(after_rest(r, long) >= after_rest(r, short));
        }

        #[test]
        fn in_range_quiet_living_rests_pass(hours in MIN_HOURS..=MAX_HOURS, since in COMBAT_QUIET_MS..u64::MAX) {
            prop_assert_eq!(check(facts(hours, false, Some(since))), Ok(()));
        }
    }
}
