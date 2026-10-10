//! Ranged shots and their hits (thuum docs/verbs/marksman.md, M2). A bow or
//! crossbow hit counts only when it claims a shot the server recorded for
//! that aggressor and weapon: unused, younger than an arrow's longest
//! flight, and from where the target is no farther than the arrow can have
//! flown since, allowing for the shot's own delivery. One hit per shot, at
//! the shot's draw power. The flight is the shooter's game's (R1): the
//! server bounds it and never simulates it.

use std::collections::{HashMap, VecDeque};

/// The fastest arrow in the lab's masters, with half again for the
/// shooter's own run: Dawnguard's steel bolt flies at 5400 units a second
/// (its PROJ), a bow's arrows at 3600 (thuum lab/esm.py over the 1.6.1170
/// masters, 2026-10-09). The draw and the bow only slow an arrow (both of
/// their multipliers are at most 1), and the player's own horizontal speed is
/// added at launch (the re-analyst's read, docs/verbs/marksman.md).
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
    power: f32,
    at_ms: u64,
    x: f32,
    y: f32,
    z: f32,
}

/// A shot as its shooter loosed it: the weapon, the draw power (0 to 1) and
/// where the shooter stood.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ShotFacts {
    /// The bow or crossbow.
    pub weapon: u32,
    /// The draw power.
    pub power: f32,
    /// Where the shooter stood.
    pub x: f32,
    /// Where the shooter stood.
    pub y: f32,
    /// Where the shooter stood.
    pub z: f32,
}

/// A ranged hit's claim.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ShotCheck {
    /// A recorded shot covers the hit, and is used up: the hit counts at the
    /// shot's draw power, 0 to 1 (the engine scales a player's arrow's
    /// damage by it once, at launch).
    Claimed {
        /// The shot's draw power.
        power: f32,
    },
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
    /// `actor` loosed `shot` at `now_ms` (a monotonic clock). A position or
    /// a power that is not a number is not recorded, so its hit is refused;
    /// a power outside 0 to 1 is held to it (the validator refuses one
    /// already).
    pub fn record(&mut self, actor: u32, shot: ShotFacts, now_ms: u64) {
        let ShotFacts { weapon, power, x, y, z } = shot;
        if !(x.is_finite() && y.is_finite() && z.is_finite() && power.is_finite()) {
            return;
        }
        let power = power.clamp(0.0, 1.0);
        let shots = self.per_actor.entry(actor).or_default();
        shots.retain(|s| now_ms.saturating_sub(s.at_ms) <= SHOT_WINDOW_MS);
        if shots.len() >= MAX_SHOTS {
            shots.pop_front();
        }
        shots.push_back(Shot { weapon, power, at_ms: now_ms, x, y, z });
    }

    /// A hit by `actor` with `weapon` on a target at (x, y, z), at `now_ms`:
    /// claims the newest unused shot of that weapon whose arrow can have
    /// reached the target since, and answers its power. Newest, because a
    /// shot that missed stays unclaimed until its window ends, and a later
    /// hit must not take its power (b-marksman 20261010-082114: a half
    /// draw's hit claimed a full-draw shot that had hit nothing); two arrows
    /// in flight at once that land out of order swap shots, both at their
    /// own draws' powers near enough.
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
        for (i, s) in shots.iter().enumerate().rev() {
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
        match found.and_then(|i| shots.remove(i)) {
            Some(shot) => ShotCheck::Claimed { power: shot.power },
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

    fn shot(weapon: u32, power: f32, x: f32, y: f32, z: f32) -> ShotFacts {
        ShotFacts { weapon, power, x, y, z }
    }

    #[test]
    fn a_hit_without_a_shot_is_refused() {
        let mut s = Shots::default();
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 0), ShotCheck::NoShot);
        s.record(2, shot(BOW, 1.0, 0.0, 0.0, 0.0), 0);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 10), ShotCheck::NoShot);
        s.record(1, shot(OTHER_BOW, 1.0, 0.0, 0.0, 0.0), 0);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 10), ShotCheck::NoShot);
    }

    #[test]
    fn a_shot_covers_one_hit() {
        let mut s = Shots::default();
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 1_000);
        assert_eq!(s.claim(1, BOW, 600.0, 0.0, 50.0, 1_200), ShotCheck::Claimed { power: 1.0 });
        assert_eq!(s.claim(1, BOW, 600.0, 0.0, 50.0, 1_300), ShotCheck::NoShot);
    }

    #[test]
    fn a_hit_farther_than_the_arrow_can_have_flown_waits_for_the_time() {
        let mut s = Shots::default();
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 0);
        // 0.1 s, and the skew's 0.3: at most 3240 + 256 units
        assert_eq!(s.claim(1, BOW, 6_000.0, 0.0, 0.0, 100), ShotCheck::TooFar);
        // 0.5 s: 6480 + 256
        assert_eq!(s.claim(1, BOW, 6_000.0, 0.0, 0.0, 500), ShotCheck::Claimed { power: 1.0 });
    }

    #[test]
    fn a_shot_resent_once_still_covers_its_hit() {
        let mut s = Shots::default();
        // the shot and its hit arrive together: 300 units in no time
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 1_000);
        assert_eq!(s.claim(1, BOW, 0.0, 300.0, 0.0, 1_000), ShotCheck::Claimed { power: 1.0 });
        // the skew's own reach, 2430 + 256 units, and no farther
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 2_000);
        assert_eq!(s.claim(1, BOW, 2_800.0, 0.0, 0.0, 2_000), ShotCheck::TooFar);
        assert_eq!(s.claim(1, BOW, 2_600.0, 0.0, 0.0, 2_000), ShotCheck::Claimed { power: 1.0 });
    }

    #[test]
    fn a_shot_lapses_after_the_longest_flight() {
        let mut s = Shots::default();
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 0);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, SHOT_WINDOW_MS + 1), ShotCheck::NoShot);
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 0);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, SHOT_WINDOW_MS), ShotCheck::Claimed { power: 1.0 });
    }

    #[test]
    fn the_newest_reaching_shot_is_claimed_first_and_a_full_quiver_drops_the_oldest() {
        let mut s = Shots::default();
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 0);
        s.record(1, shot(BOW, 1.0, 5_000.0, 0.0, 0.0), 50);
        // a target next to the first shot's spot that only the first reaches
        // in time: the second is newer but too far
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 60), ShotCheck::Claimed { power: 1.0 });
        assert_eq!(s.claim(1, BOW, 5_100.0, 0.0, 0.0, 70), ShotCheck::Claimed { power: 1.0 });
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 80), ShotCheck::NoShot);
        for (_, t) in (0..=MAX_SHOTS).zip(100_u64..) {
            s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), t);
        }
        let mut claimed = 0;
        while matches!(s.claim(1, BOW, 0.0, 0.0, 0.0, 200), ShotCheck::Claimed { .. }) {
            claimed += 1;
        }
        assert_eq!(claimed, MAX_SHOTS);
    }

    #[test]
    fn positions_that_are_not_numbers_claim_nothing() {
        let mut s = Shots::default();
        s.record(1, shot(BOW, 1.0, f32::NAN, 0.0, 0.0), 0);
        assert_eq!(s.claim(1, BOW, 0.0, 0.0, 0.0, 10), ShotCheck::NoShot);
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 0);
        assert_eq!(s.claim(1, BOW, f32::INFINITY, 0.0, 0.0, 10), ShotCheck::NoShot);
        assert_eq!(s.claim(1, BOW, 0.0, 0.0, 0.0, 10), ShotCheck::Claimed { power: 1.0 });
    }

    #[test]
    fn a_shot_that_missed_does_not_take_a_later_hits_power() {
        let mut s = Shots::default();
        // a full draw that hits nothing, then a half draw that hits
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 0);
        s.record(1, shot(BOW, 0.35, 0.0, 0.0, 0.0), 10_000);
        assert_eq!(s.claim(1, BOW, 250.0, 0.0, 0.0, 10_100), ShotCheck::Claimed { power: 0.35 });
    }

    #[test]
    fn a_hit_counts_at_its_shots_power() {
        let mut s = Shots::default();
        // a half draw's arrow and its hit, then a full draw's and its hit
        s.record(1, shot(BOW, 0.35, 0.0, 0.0, 0.0), 0);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 20), ShotCheck::Claimed { power: 0.35 });
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 1_000);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 1_020), ShotCheck::Claimed { power: 1.0 });
        // a power outside 0 to 1 is held to it; one that is not a number is
        // not a shot
        s.record(1, shot(BOW, 4.0, 0.0, 0.0, 0.0), 40);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 50), ShotCheck::Claimed { power: 1.0 });
        s.record(1, shot(BOW, f32::NAN, 0.0, 0.0, 0.0), 60);
        assert_eq!(s.claim(1, BOW, 100.0, 0.0, 0.0, 70), ShotCheck::NoShot);
    }

    #[test]
    fn a_forgotten_actor_has_no_shots() {
        let mut s = Shots::default();
        s.record(1, shot(BOW, 1.0, 0.0, 0.0, 0.0), 0);
        s.forget(1);
        assert_eq!(s.claim(1, BOW, 0.0, 0.0, 0.0, 10), ShotCheck::NoShot);
    }
}
