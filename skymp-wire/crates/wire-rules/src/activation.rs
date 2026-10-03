//! Activation reach (thuum docs/verbs/activation-reach.md). The game picks
//! what the player can activate along a ray from the eye:
//! fActivatePickLength:Interface (180) plus fActivatePickRadius (16), the
//! executable's defaults, read in the running game too (thuum lab run
//! 20261003-070107). The client can edit its INI, so the server enforces its
//! own bound: the game's reach, plus room for the eye above the feet and the
//! third-person shoulder offset (the server measures from the actor's
//! position, at its feet), plus the target's own size, since the pick lands
//! on its surface and the server knows its origin.

use crate::Verdict;

/// fActivatePickLength:Interface.
pub const PICK_LENGTH: f32 = 180.0;
/// fActivatePickRadius:Interface.
pub const PICK_RADIUS: f32 = 16.0;
/// The eye above the feet and the shoulder offset.
pub const BODY_SLACK: f32 = 256.0;

/// Whether a client's first activation of a target `distance` units away,
/// whose size (the farthest point of its bounds, times its scale) is
/// `target_size`, is within reach. A target size that is not a finite,
/// non-negative number counts as zero.
pub fn within_reach(distance: f32, target_size: f32) -> Verdict {
    let size = if target_size.is_finite() && target_size > 0.0 { target_size } else { 0.0 };
    let bound = PICK_LENGTH + PICK_RADIUS + BODY_SLACK + size;
    Verdict { allowed: distance.is_finite() && distance <= bound, bound }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_reach_is_the_pick_plus_the_body_plus_the_target() {
        assert_eq!(within_reach(0.0, 10.0).bound, 462.0);
        assert!(within_reach(462.0, 10.0).allowed);
        assert!(!within_reach(463.0, 10.0).allowed);
    }

    #[test]
    fn a_meaningless_size_or_distance_does_not_widen_the_reach() {
        assert_eq!(within_reach(0.0, f32::NAN).bound, 452.0);
        assert_eq!(within_reach(0.0, -5.0).bound, 452.0);
        assert!(!within_reach(f32::NAN, 0.0).allowed);
        assert!(!within_reach(f32::INFINITY, 0.0).allowed);
    }
}
