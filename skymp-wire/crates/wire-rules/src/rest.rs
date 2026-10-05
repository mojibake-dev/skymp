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

use serde::Deserialize;

use crate::clock::SettingsError;

/// The wait menu's range in game hours: an hour at the least, a day at most.
pub const MIN_HOURS: f32 = 1.0;
/// See [`MIN_HOURS`].
pub const MAX_HOURS: f32 = 24.0;

/// No rest within this long of a hit the player dealt or took, as the server
/// saw it: the server's stand-in for the engine's "not with enemies nearby",
/// which only the client can see. A choice, not an engine number.
pub const COMBAT_QUIET_MS: u64 = 10_000;

/// Seconds of regeneration the engine gives a rest per game hour, measured at
/// time scale 20, the default (thuum docs/verbs/rest.md): with HealRateMult
/// at 1 percent, a wait gained 0.0252 of health in an hour and 0.0506 in two
/// (run 20261004-095556), against a real-time rate of 0.007 percent a second
/// (run 20261004-100002). Neither a rest's game seconds (3,600) nor their
/// real-time equivalent (180). At time scale 10 the engine gave about 457 a
/// game hour (run 20261004-100415): a dependence on the time scale this
/// constant does not follow yet.
pub const REGEN_SECONDS_PER_HOUR: f32 = 360.0;

/// TES3MP's rest switches, from server-settings.json's `rest` block: its
/// allowWait and allowBedRest (CoreScripts 0.8.1 scripts/config.lua), the
/// latter as `allowSleep`, since Skyrim has no wilderness sleep to switch.
/// Both on, as TES3MP ships them, unless a server turns one off.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields, default)]
pub struct Switches {
    /// Players may wait.
    pub allow_wait: bool,
    /// Players may sleep in a bed.
    pub allow_sleep: bool,
}

impl Default for Switches {
    fn default() -> Self {
        Self { allow_wait: true, allow_sleep: true }
    }
}

impl Switches {
    /// The switches from the `rest` block's JSON text (`{}` for both on); the
    /// error names a bad or unknown key.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        serde_json::from_str(text).map_err(|e| SettingsError(e.to_string()))
    }
}

/// What the server knows about a player's rest.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RestFacts {
    /// The hours the client says its player rested.
    pub hours: f32,
    /// A sleep in a bed rather than a wait.
    pub sleep: bool,
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
    /// The server switched this kind of rest off.
    Off,
    /// The player is dead.
    Dead,
    /// A hit within [`COMBAT_QUIET_MS`].
    Fighting,
}

/// Whether the server lets a rest through, and why not when it does not.
pub fn check(f: RestFacts, switches: Switches) -> Result<(), Refusal> {
    if !(MIN_HOURS..=MAX_HOURS).contains(&f.hours) {
        return Err(Refusal::Hours);
    }
    if !(if f.sleep { switches.allow_sleep } else { switches.allow_wait }) {
        return Err(Refusal::Off);
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

/// A rest is a sleep when the player activated a bed no longer ago than this
/// (thuum docs/verbs/sleep.md): the bed opens the sleep menu at once, and a
/// 24-hour sleep runs about 24 s.
pub const BED_WINDOW_MS: u64 = 120_000;

/// ... and stands no farther from that bed than this: SkyMP's furniture
/// occupancy reach (MpObjectReference, kOccupationReach).
pub const BED_REACH: f32 = 256.0;

/// Game hours the Rested bonus lasts (UESP, Skyrim:Beds: "for eight in-game
/// hours").
pub const RESTED_GAME_HOURS: f32 = 8.0;

/// What the server knows about a player's last bed.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BedFacts {
    /// Milliseconds since the player's last allowed bed activation, if any.
    pub since_bed_activation_ms: Option<u64>,
    /// The player's distance to that bed, in units.
    pub distance_to_bed: f32,
}

/// Whether a rest was a sleep. The server decides from its own facts; the
/// client's flag comes from a furniture test vanilla beds never pass.
#[must_use]
pub fn slept(f: BedFacts) -> bool {
    f.since_bed_activation_ms.is_some_and(|ms| ms <= BED_WINDOW_MS)
        && f.distance_to_bed.is_finite()
        && f.distance_to_bed <= BED_REACH
}

/// How long the Rested bonus a sleep earns lasts, in real milliseconds at the
/// clock's `time_scale` (game seconds a real one); none after a wait, and none
/// at a time scale that is not a positive number. M1 has no owned beds and no
/// marriage, so a sleep earns Rested, never Well Rested or Lover's Comfort.
#[must_use]
pub fn rested_ms(slept: bool, time_scale: f32) -> Option<u64> {
    if !slept || !time_scale.is_finite() || time_scale <= 0.0 {
        return None;
    }
    let ms = f64::from(RESTED_GAME_HOURS) * 3_600_000.0 / f64::from(time_scale);
    if !(1.0..=f64::from(u32::MAX)).contains(&ms) {
        return None;
    }
    #[allow(clippy::as_conversions, clippy::cast_possible_truncation, clippy::cast_sign_loss)] // range checked above
    Some(ms as u64)
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    const fn bed(since: Option<u64>, distance: f32) -> BedFacts {
        BedFacts { since_bed_activation_ms: since, distance_to_bed: distance }
    }

    #[test]
    fn a_rest_at_a_bed_just_activated_is_a_sleep() {
        assert!(slept(bed(Some(0), 0.0)));
        assert!(slept(bed(Some(30_000), 100.0)));
        assert!(slept(bed(Some(BED_WINDOW_MS), BED_REACH)));
    }

    #[test]
    fn a_rest_without_a_bed_or_away_from_it_is_a_wait() {
        assert!(!slept(bed(None, 0.0)));
        assert!(!slept(bed(Some(BED_WINDOW_MS + 1), 0.0)));
        assert!(!slept(bed(Some(1_000), BED_REACH + 1.0)));
        assert!(!slept(bed(Some(1_000), f32::NAN)));
    }

    #[test]
    fn a_sleep_earns_eight_game_hours_of_rested() {
        // eight game hours at time scale 20: 24 real minutes (UESP)
        assert_eq!(rested_ms(true, 20.0), Some(1_440_000));
        assert_eq!(rested_ms(true, 10.0), Some(2_880_000));
        assert_eq!(rested_ms(false, 20.0), None);
        assert_eq!(rested_ms(true, 0.0), None);
        assert_eq!(rested_ms(true, f32::NAN), None);
    }

    const fn facts(hours: f32, dead: bool, since: Option<u64>) -> RestFacts {
        RestFacts { hours, sleep: false, is_dead: dead, since_last_hit_ms: since }
    }

    fn on() -> Switches {
        Switches::default()
    }

    #[test]
    fn the_menu_range_alive_and_quiet() {
        assert_eq!(check(facts(1.0, false, None), on()), Ok(()));
        assert_eq!(check(facts(24.0, false, Some(10_000)), on()), Ok(()));
        assert_eq!(check(facts(0.5, false, None), on()), Err(Refusal::Hours));
        assert_eq!(check(facts(24.01, false, None), on()), Err(Refusal::Hours));
        assert_eq!(check(facts(f32::NAN, false, None), on()), Err(Refusal::Hours));
        assert_eq!(check(facts(f32::INFINITY, false, None), on()), Err(Refusal::Hours));
        assert_eq!(check(facts(2.0, true, None), on()), Err(Refusal::Dead));
        assert_eq!(check(facts(2.0, false, Some(9_999)), on()), Err(Refusal::Fighting));
    }

    #[test]
    fn the_switches_turn_one_kind_off() {
        let no_wait = Switches::from_json(r#"{"allowWait": false}"#).unwrap_or_default();
        let sleep = RestFacts { sleep: true, ..facts(8.0, false, None) };
        assert_eq!(check(facts(8.0, false, None), no_wait), Err(Refusal::Off));
        assert_eq!(check(sleep, no_wait), Ok(()));
        let no_sleep = Switches::from_json(r#"{"allowSleep": false}"#).unwrap_or_default();
        assert_eq!(check(sleep, no_sleep), Err(Refusal::Off));
        assert_eq!(check(facts(8.0, false, None), no_sleep), Ok(()));
        assert_eq!(Switches::from_json("{}").ok(), Some(on()));
        assert!(Switches::from_json(r#"{"allowWildernessRest": true}"#).is_err());
        assert!(Switches::from_json(r#"{"allowWait": 1}"#).is_err());
    }

    #[test]
    fn an_hour_restores_health_fully() {
        // UESP's out-of-combat health rate, 0.70 percent a second, at a 100
        // percent multiplier: half health is full after the menu's hour
        let health = Regen { percentage: 0.5, rate: 0.7, rate_mult: 100.0 };
        assert!((after_rest(health, 1.0) - 1.0).abs() < f32::EPSILON);
    }

    #[test]
    fn the_engine_s_lowered_rate() {
        // rest.md, the measured runs: HealRateMult 1 percent from half health
        // gains 0.0252 in an hour and 0.0504 in two
        let lowered = Regen { percentage: 0.5, rate: 0.7, rate_mult: 1.0 };
        assert!((after_rest(lowered, 1.0) - 0.5252).abs() < 1e-5);
        assert!((after_rest(lowered, 2.0) - 0.5504).abs() < 1e-5);
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
            prop_assert_eq!(check(facts(hours, false, Some(since)), on()), Ok(()));
        }
    }
}
