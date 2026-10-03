//! Melee reach (thuum docs/verbs/melee-reach.md). A swing lands on the target
//! whose center lies within the attacker's reach plus both actors' forward
//! body extents (the engine's pick), or that a sphere cast from the
//! attacker's eye meets when the pick finds nothing (thuum
//! ghidra/notes/melee-reach-1-7-104.md; the edge measured in the lab, runs
//! 20261003-084251 and -084645). Reach is the attacker's scale times
//! fCombatBashReach (a bash), fCombatDistance times the weapon's reach, or
//! the race's unarmed reach; the server takes the largest the attacker's
//! equipment allows, since the bash flag is the client's claim.

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

    proptest! {
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
