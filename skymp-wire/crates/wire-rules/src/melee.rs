//! Melee reach (thuum docs/verbs/melee-reach.md). A swing lands on the target
//! whose center lies within the attacker's reach plus both actors' forward
//! body extents (the engine's pick), or that a sphere cast from the
//! attacker's eye meets when the pick finds nothing (thuum
//! ghidra/notes/melee-reach-1-7-104.md; the edge measured in the lab, runs
//! 20261003-084251 and -084645). Reach is the attacker's scale times
//! fCombatBashReach (a bash), fCombatDistance times the weapon's reach, or
//! the race's unarmed reach; the server takes the largest the attacker's
//! equipment allows, since the bash flag is the client's claim.
//!
//! The hit cone (thuum docs/verbs/hit-cone.md): the engine lands a swing only
//! on a target within its cone half angle of the attacker's heading, in the
//! ground plane (Address Library 47297, read in Ghidra). The lab measured a
//! player's swing landing at 30 degrees off and missing at 45 (run
//! 20261004-045109), which is fCombatHitConeAngle's 35 with the target's
//! width, and landing at any angle once a console raises that setting (run
//! 20261004-045647): the engine's cone is the client's to widen, so the
//! server holds the angle.

use crate::Verdict;

/// fObjectHitWeaponReach (150) plus fHitCasterSizeSmall (12): executable
/// defaults read live in the lab, the longest of the eye casts.
pub const EYE_CAST_REACH: f32 = 150.0 + 12.0;
/// Half the actor bound's length (28, read live), before scale.
pub const FORWARD_EXTENT: f32 = 14.0;
/// Positions reach the server every 130 ms per actor (skymp5-client
/// sendInputsService.ts): room for two actors moving at sprint speed since
/// their last report.
pub const STALE_SLACK: f32 = 256.0;

/// fCombatHitConeAngle (35): the cone half angle of a swing without attack
/// data, an executable default read live in the lab (run 20261004-045647).
pub const HIT_CONE_ANGLE: f32 = 35.0;
/// fMeleeSweepViewAngleMult (2): a sweep's extra targets get twice the cone,
/// read live in the same run. Only a power attack sweeps.
pub const SWEEP_MULT: f32 = 2.0;
/// The server's heading for the attacker is its last movement report,
/// 130 ms old at worst (skymp5-client sendInputsService.ts): room for a
/// flick of the mouse since, and for the player's aim offset, which the
/// engine adds to the heading and the server does not see.
pub const STALE_HEADING_SLACK: f32 = 45.0;

/// What the server knows about the direction of a player's melee hit.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ConeFacts {
    /// The attacker's heading as the server records it, degrees (0 north, +y;
    /// clockwise).
    pub heading: f32,
    /// Target minus attacker, east (+x) and north (+y), units.
    pub dx: f32,
    /// Target minus attacker, north.
    pub dy: f32,
    /// The widest strike angle among the attacker race's forward attacks
    /// (RACE ATKD with an attack angle of 0; 50 for the playable races but
    /// DarkElfRace's 35), 0 for none. Update.esm's mounted side attacks
    /// (attack angle 90 or -90, strike 85) are left out: mounted combat is
    /// M7.
    pub widest_strike_angle: f32,
    /// The hit is a power attack, as the server kept the flag
    /// (docs/verbs/damage-flags.md).
    pub power: bool,
    /// The target is dead (the engine doubles the cone; damage to the dead
    /// does not count).
    pub target_dead: bool,
}

/// The largest angle off the attacker's heading the server lets a hit land
/// at, degrees.
pub fn cone_bound(f: ConeFacts) -> f32 {
    let widest = HIT_CONE_ANGLE.max(finite_or_zero(f.widest_strike_angle));
    widest * if f.power { SWEEP_MULT } else { 1.0 } + STALE_HEADING_SLACK
}

/// Whether the hit is within the cone. A target on top of the attacker has
/// no direction and passes, as does a dead one; a heading that is not a
/// number never does.
pub fn within_cone(f: ConeFacts) -> Verdict {
    let bound = cone_bound(f);
    if f.target_dead || (f.dx == 0.0 && f.dy == 0.0) {
        return Verdict { allowed: true, bound };
    }
    // the engine's yaw: atan(x / y) with the quadrant fixed (ID 70173)
    let bearing = f.dx.atan2(f.dy).to_degrees();
    let off = (f.heading - bearing).rem_euclid(360.0);
    let angle = off.min(360.0 - off);
    Verdict { allowed: angle <= bound, bound }
}

/// What the server knows about a player's melee hit on another player.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MeleeFacts {
    /// Center to center, units.
    pub distance: f32,
    /// fCombatDistance (Skyrim.esm: 141).
    pub combat_distance: f32,
    /// fCombatBashReach (Skyrim.esm: 141).
    pub bash_reach: f32,
    /// The longest DNAM reach among the attacker's worn weapons, 0 for none.
    pub weapon_reach: f32,
    /// The attacker race's unarmed reach (RACE DATA; 96 for the playable
    /// races).
    pub unarmed_reach: f32,
    /// The attacker's scale: its race's height for its sex (refScale and the
    /// base record's height are 1 for player characters).
    pub aggressor_scale: f32,
    /// The target's scale, likewise.
    pub target_scale: f32,
}

fn finite_or_zero(v: f32) -> f32 {
    if v.is_finite() && v > 0.0 { v } else { 0.0 }
}

/// The largest center distance the game lets the attacker hit from, plus
/// the room for stale positions.
pub fn bound(f: MeleeFacts) -> f32 {
    let scale_a = finite_or_zero(f.aggressor_scale);
    let scale_t = finite_or_zero(f.target_scale);
    let base = finite_or_zero(f.bash_reach)
        .max(finite_or_zero(f.combat_distance) * finite_or_zero(f.weapon_reach))
        .max(finite_or_zero(f.unarmed_reach));
    let reach = (scale_a * base).max(EYE_CAST_REACH);
    reach + FORWARD_EXTENT * (scale_a + scale_t) + STALE_SLACK
}

/// Whether the hit is within reach.
pub fn within_reach(f: MeleeFacts) -> Verdict {
    let bound = bound(f);
    Verdict { allowed: f.distance.is_finite() && f.distance <= bound, bound }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    fn nord_with(weapon_reach: f32, distance: f32) -> MeleeFacts {
        MeleeFacts {
            distance,
            combat_distance: 141.0,
            bash_reach: 141.0,
            weapon_reach,
            unarmed_reach: 96.0,
            aggressor_scale: 1.03,
            target_scale: 1.03,
        }
    }

    #[test]
    fn a_nord_with_a_sword_reaches_447() {
        let b = bound(nord_with(1.0, 0.0));
        assert!((b - 446.84).abs() < 0.01, "{b}");
        assert!(within_reach(nord_with(1.0, 400.0)).allowed);
        assert!(!within_reach(nord_with(1.0, 500.0)).allowed);
    }

    #[test]
    fn a_greatsword_reaches_farther() {
        // 141 * 1.3 * 1.03 + 2 * 14 * 1.03 + 256
        let b = bound(nord_with(1.3, 0.0));
        assert!((b - 473.63).abs() < 0.01, "{b}");
    }

    #[test]
    fn every_measured_hit_is_inside_the_bound() {
        // the lab's hits (sword to 185, greatsword to 220) never refused
        assert!(within_reach(nord_with(1.0, 185.0)).allowed);
        assert!(within_reach(nord_with(1.3, 220.0)).allowed);
    }

    fn nord_facing(heading: f32, bearing: f32, power: bool) -> ConeFacts {
        let (s, c) = bearing.to_radians().sin_cos();
        ConeFacts { heading, dx: 120.0 * s, dy: 120.0 * c, widest_strike_angle: 50.0, power, target_dead: false }
    }

    #[test]
    fn the_bound_is_the_race_s_widest_cone_doubled_for_a_sweep_plus_slack() {
        assert!((cone_bound(nord_facing(0.0, 0.0, false)) - 95.0).abs() < 1e-4);
        assert!((cone_bound(nord_facing(0.0, 0.0, true)) - 145.0).abs() < 1e-4);
        let mut dunmer = nord_facing(0.0, 0.0, false);
        dunmer.widest_strike_angle = 35.0;
        assert!((cone_bound(dunmer) - 80.0).abs() < 1e-4);
        // a race without attack data still gets fCombatHitConeAngle
        dunmer.widest_strike_angle = 0.0;
        assert!((cone_bound(dunmer) - 80.0).abs() < 1e-4);
    }

    #[test]
    fn the_lab_s_swings_land_and_a_hit_behind_does_not() {
        // probe 1: the engine landed at 0 and 30 degrees off the heading
        for heading in [0.0, 30.0, 330.0] {
            assert!(within_cone(nord_facing(heading, 0.0, false)).allowed, "{heading}");
        }
        // probe 2: with the cone raised by a console, the engine landed at
        // 135 and 180 degrees; the server refuses both
        for heading in [135.0, 180.0, 225.0] {
            assert!(!within_cone(nord_facing(heading, 0.0, false)).allowed, "{heading}");
        }
        // a power attack's sweep reaches farther round, not behind
        assert!(within_cone(nord_facing(135.0, 0.0, true)).allowed);
        assert!(!within_cone(nord_facing(180.0, 0.0, true)).allowed);
    }

    #[test]
    fn headings_wrap_and_bearings_follow_the_engine_s_yaw() {
        // c2 due east of a c1 facing east (90), then due west of it
        assert!(within_cone(nord_facing(90.0, 90.0, false)).allowed);
        assert!(!within_cone(nord_facing(90.0, 270.0, false)).allowed);
        // 359 and 1 are 2 apart
        assert!(within_cone(nord_facing(359.0, 1.0, false)).allowed);
        assert!(within_cone(nord_facing(-30.0, 0.0, false)).allowed);
    }

    #[test]
    fn the_dead_and_the_directionless_pass_and_a_meaningless_heading_does_not() {
        let mut f = nord_facing(180.0, 0.0, false);
        f.target_dead = true;
        assert!(within_cone(f).allowed);
        let f = ConeFacts { dx: 0.0, dy: 0.0, ..nord_facing(180.0, 0.0, false) };
        assert!(within_cone(f).allowed);
        assert!(!within_cone(nord_facing(f32::NAN, 0.0, false)).allowed);
    }

    proptest! {
        #[test]
        fn the_cone_is_symmetric_and_the_angle_alone_decides(heading in -720.0f32..720.0, off in 0.0f32..180.0) {
            let left = within_cone(nord_facing(heading, heading - off, false));
            let right = within_cone(nord_facing(heading, heading + off, false));
            // near the edge single precision may split the two; elsewhere they agree
            if (off - 95.0).abs() > 0.01 {
                prop_assert_eq!(left.allowed, off <= 95.0);
                prop_assert_eq!(right.allowed, off <= 95.0);
            }
        }

        #[test]
        fn a_meaningless_strike_angle_never_widens_past_the_sweep(junk in prop::num::f32::ANY) {
            let mut f = nord_facing(0.0, 0.0, true);
            f.widest_strike_angle = junk;
            let b = cone_bound(f);
            prop_assert!(!b.is_nan());
            prop_assert!(b >= HIT_CONE_ANGLE * SWEEP_MULT + STALE_HEADING_SLACK);
        }

        #[test]
        fn a_meaningless_weapon_reach_never_yields_a_meaningless_bound(junk in prop::num::f32::ANY) {
            let mut f = nord_with(1.0, 0.0);
            f.weapon_reach = junk;
            prop_assert!(!bound(f).is_nan());
            prop_assert!(bound(f) >= bound(nord_with(0.0, 0.0)));
        }

        #[test]
        fn the_distance_alone_decides_against_a_fixed_bound(d in prop::num::f32::ANY) {
            let b = bound(nord_with(1.0, 0.0));
            prop_assert_eq!(within_reach(nord_with(1.0, d)).allowed, d.is_finite() && d <= b);
        }
    }
}
