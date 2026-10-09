//! Ranged shots and their hits (thuum docs/verbs/marksman.md, M2). A bow or
//! crossbow hit counts only when it claims a shot the server recorded for
//! that aggressor and weapon: unused, younger than an arrow's longest
//! flight, and from where the target is no farther than the arrow can have
//! flown since, allowing for the shot's own delivery. One hit per shot. The
//! flight is the shooter's game's (R1): the server bounds it and never
//! simulates it.

use std::collections::{HashMap, VecDeque};

/// The fastest arrow in the lab's masters, with half again for what a bow
/// or the draw may add (not measured; the verb doc's HYPOTHESIS): Dawnguard's
/// steel bolt flies at 5400 units a second (its PROJ), a bow's arrows at 3600
/// (thuum lab/esm.py over the 1.6.1170 masters, 2026-10-09).
pub const MAX_ARROW_SPEED: f32 = 5400.0 * 1.5;

/// How long a shot waits for its hit: an arrow's PROJ range is 60000 units,
/// which the slowest arrow at full speed (3600 a second) covers in under
/// 17 seconds.
pub const SHOT_WINDOW_MS: u64 = 17_000;

/// Slack on the distance an arrow can have flown, units: the two positions'
/// age (a movement report every 130 ms, at a horse's sprint) and the
/// target's size.
pub const RANGE_SLACK: f32 = 256.0;

/// How much the time between a shot's arrival and its hit's can understate
/// the arrow's flight, milliseconds. Both ride the client's one ordered
/// channel, so a shot lost on the way is resent after 300 ms (wire-transport
/// `Limits::resend_ms`) while its hit, sent later, waits behind it and
/// arrives with it. One resend is allowed: a hit within about 2700 units
/// never waits on time, and a shot lost twice may lose a far hit.
pub const DELIVERY_SKEW_MS: u64 = 300;

/// Shots kept per actor; a quiver loosed faster than its hits arrive drops
/// the oldest.
pub const MAX_SHOTS: usize = 16;

#[derive(Debug, Clone, Copy, PartialEq)]
struct Shot {
    weapon: u32,
    at_ms: u64,
    x: f32,
    y: f32,
    z: f32,
}

/// A ranged hit's claim.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShotCheck {
    /// A recorded shot covers the hit, and is used up.
    Claimed,
    /// No unused shot of that weapon within the window.
    NoShot,
    /// Shots of that weapon wait, but none can have reached the target yet.
    TooFar,
}

/// Every actor's recent shots.
#[derive(Debug, Default)]
pub struct Shots {
    per_actor: HashMap<u32, VecDeque<Shot>>,
}

impl Shots {
    /// `actor` loosed an arrow from `weapon`, standing at (x, y, z), at
    /// `now_ms` (a monotonic clock). A position that is not a number is
    /// not recorded, so its hit is refused.
    pub fn record(&mut self, actor: u32, weapon: u32, x: f32, y: f32, z: f32, now_ms: u64) {
        if !(x.is_finite() && y.is_finite() && z.is_finite()) {
            return;
        }
        let shots = self.per_actor.entry(actor).or_default();
        shots.retain(|s| now_ms.saturating_sub(s.at_ms) <= SHOT_WINDOW_MS);
        if shots.len() >= MAX_SHOTS {
            shots.pop_front();
        }
        shots.push_back(Shot { weapon, at_ms: now_ms, x, y, z });
    }

    /// A hit by `actor` with `weapon` on a target at (x, y, z), at `now_ms`:
    /// claims the oldest unused shot of that weapon whose arrow can have
    /// reached the target since.
    pub fn claim(&mut self, actor: u32, weapon: u32, x: f32, y: f32, z: f32, now_ms: u64) -> ShotCheck {
        let Some(shots) = self.per_actor.get_mut(&actor) else {
            return ShotCheck::NoShot;
        };
        shots.retain(|s| now_ms.saturating_sub(s.at_ms) <= SHOT_WINDOW_MS);
        if !(x.is_finite() && y.is_finite() && z.is_finite()) {
            return ShotCheck::NoShot;
        }
        let mut any = false;
        let mut found = None;
        for (i, s) in shots.iter().enumerate() {
            if s.weapon != weapon {
                continue;
            }
            any = true;
            // within the window and the skew, which fit a u16 of milliseconds
            let flown_ms = now_ms
                .saturating_sub(s.at_ms)
                .min(SHOT_WINDOW_MS)
                .saturating_add(DELIVERY_SKEW_MS);
            let flown_s = f32::from(u16::try_from(flown_ms).unwrap_or(u16::MAX)) / 1000.0;
            let reach = MAX_ARROW_SPEED * flown_s + RANGE_SLACK;
            let (dx, dy, dz) = (x - s.x, y - s.y, z - s.z);
            if (dx * dx + dy * dy + dz * dz).sqrt() <= reach {
                found = Some(i);
                break;
            }
        }
        match found {
            Some(i) => {
                shots.remove(i);
                ShotCheck::Claimed
            }
            None if any => ShotCheck::TooFar,
            None => ShotCheck::NoShot,
        }
    }

    /// The actor is gone: its shots with it.
    pub fn forget(&mut self, actor: u32) {
        self.per_actor.remove(&actor);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const BOW: u32 = 0x3b562;
    const OTHER_BOW: u32 = 0x13985;

    #[test]
    fn a_hit_without_a_shot_is_refused() {
        let mut s = Shots::default();
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 0), ShotCheck::NoShot);
        s.record(2, BOW, 0.0, 0.0, 0.0, 0);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 10), ShotCheck::NoShot);
        s.record(1, OTHER_BOW, 0.0, 0.0, 0.0, 0);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 10), ShotCheck::NoShot);
    }

    #[test]
    fn a_shot_covers_one_hit() {
        let mut s = Shots::default();
        s.record(1, BOW, 0.0, 0.0, 0.0, 1_000);
        assert_eq!(s.claim(1, BOW, 600.0, 0.0, 50.0, 1_200), ShotCheck::Claimed);
        assert_eq!(s.claim(1, BOW, 600.0, 0.0, 50.0, 1_300), ShotCheck::NoShot);
    }

    #[test]
    fn a_hit_farther_than_the_arrow_can_have_flown_waits_for_the_time() {
        let mut s = Shots::default();
        s.record(1, BOW, 0.0, 0.0, 0.0, 0);
        // 0.1 s, and the skew's 0.3: at most 3240 + 256 units
        assert_eq!(s.claim(1, BOW, 6_000.0, 0.0, 0.0, 100), ShotCheck::TooFar);
        // 0.5 s: 6480 + 256
        assert_eq!(s.claim(1, BOW, 6_000.0, 0.0, 0.0, 500), ShotCheck::Claimed);
    }

    #[test]
    fn a_shot_resent_once_still_covers_its_hit() {
        let mut s = Shots::default();
        // the shot and its hit arrive together: 300 units in no time
        s.record(1, BOW, 0.0, 0.0, 0.0, 1_000);
        assert_eq!(s.claim(1, BOW, 0.0, 300.0, 0.0, 1_000), ShotCheck::Claimed);
        // the skew's own reach, 2430 + 256 units, and no farther
        s.record(1, BOW, 0.0, 0.0, 0.0, 2_000);
        assert_eq!(s.claim(1, BOW, 2_800.0, 0.0, 0.0, 2_000), ShotCheck::TooFar);
        assert_eq!(s.claim(1, BOW, 2_600.0, 0.0, 0.0, 2_000), ShotCheck::Claimed);
    }

    #[test]
    fn a_shot_lapses_after_the_longest_flight() {
        let mut s = Shots::default();
        s.record(1, BOW, 0.0, 0.0, 0.0, 0);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, SHOT_WINDOW_MS + 1), ShotCheck::NoShot);
        s.record(1, BOW, 0.0, 0.0, 0.0, 0);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, SHOT_WINDOW_MS), ShotCheck::Claimed);
    }

    #[test]
    fn the_oldest_reaching_shot_is_claimed_first_and_a_full_quiver_drops_the_oldest() {
        let mut s = Shots::default();
        s.record(1, BOW, 0.0, 0.0, 0.0, 0);
        s.record(1, BOW, 5_000.0, 0.0, 0.0, 50);
        // only the second reaches a target next to it yet
        assert_eq!(s.claim(1, BOW, 5_100.0, 0.0, 0.0, 60), ShotCheck::Claimed);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 70), ShotCheck::Claimed);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 80), ShotCheck::NoShot);
        for (_, t) in (0..=MAX_SHOTS).zip(100_u64..) {
            s.record(1, BOW, 0.0, 0.0, 0.0, t);
        }
        let mut claimed = 0;
        while s.claim(1, BOW, 0.0, 0.0, 0.0, 200) == ShotCheck::Claimed {
            claimed += 1;
        }
        assert_eq!(claimed, MAX_SHOTS);
    }

    #[test]
    fn positions_that_are_not_numbers_claim_nothing() {
        let mut s = Shots::default();
        s.record(1, BOW, f32::NAN, 0.0, 0.0, 0);
        assert_eq!(s.claim(1, BOW, 0.0, 0.0, 0.0, 10), ShotCheck::NoShot);
        s.record(1, BOW, 0.0, 0.0, 0.0, 0);
        assert_eq!(s.claim(1, BOW, f32::INFINITY, 0.0, 0.0, 10), ShotCheck::NoShot);
        assert_eq!(s.claim(1, BOW, 0.0, 0.0, 0.0, 10), ShotCheck::Claimed);
    }

    #[test]
    fn a_forgotten_actor_has_no_shots() {
        let mut s = Shots::default();
        s.record(1, BOW, 0.0, 0.0, 0.0, 0);
        s.forget(1);
        assert_eq!(s.claim(1, BOW, 0.0, 0.0, 0.0, 10), ShotCheck::NoShot);
    }
}
