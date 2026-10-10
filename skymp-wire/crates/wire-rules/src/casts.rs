//! Spell casts and their hits (thuum docs/verbs/spell-cast.md, M2). A spell
//! hit on an actor counts only when it claims a cast the server recorded for
//! that caster and spell. A fire-and-forget cast covers its first hit, and an
//! area spell's other targets within the splash window; a concentration cast
//! covers every hit while it is open, each counted by the time since the
//! cast's last counted hit, so its damage follows the time the stream held a
//! target and not how often the caster's game reports it (five reports a
//! second for Flames in x-spell-probe and x-spell-probe2, 2026-10-09). The
//! cast and the projectile's flight are the caster's game's (R1): the server
//! bounds them and never simulates them.

use std::collections::{HashMap, VecDeque};

/// How long a fire-and-forget cast waits for its hit, milliseconds: as an
/// arrow's (wire-rules ranged), the longest flight the masters allow.
pub const CAST_WINDOW_MS: u64 = 17_000;

/// Slack on a hit's distance from the caster, units: the two positions' age
/// and the target's size, as an arrow's.
pub const RANGE_SLACK: f32 = 256.0;

/// The most a concentration hit counts, seconds: a report after a longer gap
/// (the stream's way to its target, a report lost) counts this much, so a
/// stream never counts time it did not hold its target.
pub const MAX_TICK_MS: u64 = 250;

/// How long after an area spell's first hit its other targets may claim the
/// same cast, milliseconds: the server's splash window for weapons
/// (ActionListener::OnWeaponHit, kSplashTimeWindow).
pub const SPLASH_MS: u64 = 100;

/// The most targets one area cast covers: the server's splash limit for
/// weapons (kMaxSplashTargets).
pub const MAX_SPLASH_TARGETS: usize = 4;

/// Casts kept per actor; older ones go first.
pub const MAX_CASTS: usize = 16;

/// How a spell is cast (the record's SPIT casting type).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CastKind {
    /// One release, one projectile or one touch.
    FireAndForget,
    /// A stream held while the hand casts.
    Concentration,
}

/// A cast as its caster's game made it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CastFacts {
    /// The spell.
    pub spell: u32,
    /// The hand or slot: 0 left, 1 right, 2 voice, 3 instant.
    pub hand: u8,
    /// Fire-and-forget or concentration.
    pub kind: CastKind,
    /// Whether the spell hits an area (its costliest effect's area above 0).
    pub area: bool,
    /// How far from the caster its hit can be, units: its projectile's
    /// range, read from the records by the caller.
    pub reach: f32,
    /// Where the caster stood.
    pub x: f32,
    /// Where the caster stood.
    pub y: f32,
    /// Where the caster stood.
    pub z: f32,
}

/// A spell hit as the caster's game reported it: the spell, the target's
/// form id and where the server has the target.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CastHit {
    /// The spell.
    pub spell: u32,
    /// The target's form id.
    pub target: u32,
    /// Where the target stands.
    pub x: f32,
    /// Where the target stands.
    pub y: f32,
    /// Where the target stands.
    pub z: f32,
}

/// A spell hit's claim.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum CastCheck {
    /// A recorded cast covers the hit: the damage counts `scale` times the
    /// spell's magnitude (1 for a fire-and-forget hit; seconds for a
    /// concentration hit, whose magnitude is per second).
    Claimed {
        /// The factor on the spell's magnitude.
        scale: f32,
    },
    /// No cast of that spell covers the hit.
    NoCast,
    /// Casts of that spell wait, but the target is beyond their reach.
    TooFar,
}

#[derive(Debug, Clone, Copy, PartialEq)]
struct Released {
    facts: CastFacts,
    at_ms: u64,
    /// The first hit's time and every target it covered.
    first_hit_ms: Option<u64>,
    targets: [u32; MAX_SPLASH_TARGETS],
    hits: usize,
}

#[derive(Debug, Clone, Copy, PartialEq)]
struct Open {
    facts: CastFacts,
    last_counted_ms: u64,
}

#[derive(Debug, Default)]
struct ActorCasts {
    released: VecDeque<Released>,
    open: [Option<Open>; 4],
}

/// Every actor's recent casts.
#[derive(Debug, Default)]
pub struct Casts {
    per_actor: HashMap<u32, ActorCasts>,
}

fn finite(facts: &CastFacts) -> bool {
    facts.x.is_finite() && facts.y.is_finite() && facts.z.is_finite() && facts.reach.is_finite()
}

fn within(facts: &CastFacts, x: f32, y: f32, z: f32) -> bool {
    let (dx, dy, dz) = (x - facts.x, y - facts.y, z - facts.z);
    (dx * dx + dy * dy + dz * dz).sqrt() <= facts.reach.max(0.0) + RANGE_SLACK
}

impl Casts {
    /// `actor` began a cast at `now_ms` (a monotonic clock): a
    /// fire-and-forget one waits for its hit, a concentration one opens in
    /// its hand, ending any stream that hand held. A cast whose position or
    /// reach is not a number, or from a hand outside 0 to 3, is not recorded,
    /// so its hit is refused.
    pub fn start(&mut self, actor: u32, facts: CastFacts, now_ms: u64) {
        let hand = usize::from(facts.hand);
        if hand >= 4 || !finite(&facts) {
            return;
        }
        let casts = self.per_actor.entry(actor).or_default();
        match facts.kind {
            CastKind::FireAndForget => {
                casts.released.retain(|c| now_ms.saturating_sub(c.at_ms) <= CAST_WINDOW_MS);
                if casts.released.len() >= MAX_CASTS {
                    casts.released.pop_front();
                }
                casts.released.push_back(Released {
                    facts,
                    at_ms: now_ms,
                    first_hit_ms: None,
                    targets: [0; MAX_SPLASH_TARGETS],
                    hits: 0,
                });
            }
            CastKind::Concentration => {
                if let Some(slot) = casts.open.get_mut(hand) {
                    *slot = Some(Open { facts, last_counted_ms: now_ms });
                }
            }
        }
    }

    /// `actor`'s stream in `hand` ended (its stop, or the server's own end of
    /// it).
    pub fn end(&mut self, actor: u32, hand: u8) {
        if let Some(slot) = self
            .per_actor
            .get_mut(&actor)
            .and_then(|c| c.open.get_mut(usize::from(hand)))
        {
            *slot = None;
        }
    }

    /// Whether `actor` holds a stream open in `hand`.
    pub fn is_open(&self, actor: u32, hand: u8) -> bool {
        self.per_actor
            .get(&actor)
            .and_then(|c| c.open.get(usize::from(hand)))
            .is_some_and(Option::is_some)
    }

    /// A hit by `actor` at `now_ms`: an open stream of the hit's spell
    /// counts the time since its last counted hit, at most MAX_TICK_MS;
    /// otherwise the newest fire-and-forget cast of that spell that can still
    /// take a hit (unused, or an area cast's further target within SPLASH_MS
    /// of its first) and whose reach covers the target, once per target.
    pub fn claim(&mut self, actor: u32, hit: CastHit, now_ms: u64) -> CastCheck {
        let CastHit { spell, target, x, y, z } = hit;
        let Some(casts) = self.per_actor.get_mut(&actor) else {
            return CastCheck::NoCast;
        };
        if !(x.is_finite() && y.is_finite() && z.is_finite()) {
            return CastCheck::NoCast;
        }
        let mut any = false;
        for open in casts.open.iter_mut().flatten() {
            if open.facts.spell != spell {
                continue;
            }
            any = true;
            if within(&open.facts, x, y, z) {
                let tick_ms = now_ms.saturating_sub(open.last_counted_ms).min(MAX_TICK_MS);
                open.last_counted_ms = now_ms;
                let scale = f32::from(u16::try_from(tick_ms).unwrap_or(u16::MAX)) / 1000.0;
                return CastCheck::Claimed { scale };
            }
        }
        casts.released.retain(|c| now_ms.saturating_sub(c.at_ms) <= CAST_WINDOW_MS);
        for cast in casts.released.iter_mut().rev() {
            if cast.facts.spell != spell {
                continue;
            }
            let available = match cast.first_hit_ms {
                None => true,
                Some(first) => {
                    cast.facts.area
                        && now_ms.saturating_sub(first) <= SPLASH_MS
                        && cast.hits < MAX_SPLASH_TARGETS
                        && !cast.targets.iter().take(cast.hits).any(|t| *t == target)
                }
            };
            if !available {
                continue;
            }
            any = true;
            if !within(&cast.facts, x, y, z) {
                continue;
            }
            if let Some(slot) = cast.targets.get_mut(cast.hits) {
                *slot = target;
            }
            cast.hits = cast.hits.saturating_add(1);
            cast.first_hit_ms.get_or_insert(now_ms);
            return CastCheck::Claimed { scale: 1.0 };
        }
        if any {
            CastCheck::TooFar
        } else {
            CastCheck::NoCast
        }
    }

    /// The actor is gone (dead, left): its casts and streams with it.
    pub fn forget(&mut self, actor: u32) {
        self.per_actor.remove(&actor);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Skyrim.esm's Flames (lab/esm.py, docs/verbs/favorites.md); the other
    // two are labels, not records
    const FLAMES: u32 = 0x12fcd;
    const BOLT: u32 = 0xa001;
    const BALL: u32 = 0xa002;

    fn hit(spell: u32, target: u32, x: f32, y: f32, z: f32) -> CastHit {
        CastHit { spell, target, x, y, z }
    }

    fn cast(spell: u32, hand: u8, kind: CastKind, area: bool) -> CastFacts {
        CastFacts { spell, hand, kind, area, reach: 1_000.0, x: 0.0, y: 0.0, z: 0.0 }
    }

    #[test]
    fn a_hit_without_a_cast_is_refused() {
        let mut c = Casts::default();
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), 0), CastCheck::NoCast);
        c.start(1, cast(BALL, 1, CastKind::FireAndForget, true), 0);
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), 10), CastCheck::NoCast);
    }

    #[test]
    fn a_fire_and_forget_cast_covers_one_hit() {
        let mut c = Casts::default();
        c.start(1, cast(BOLT, 1, CastKind::FireAndForget, false), 0);
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), 50), CastCheck::Claimed { scale: 1.0 });
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), 60), CastCheck::NoCast);
    }

    #[test]
    fn an_area_cast_covers_its_other_targets_once_each_within_the_splash() {
        let mut c = Casts::default();
        c.start(1, cast(BALL, 1, CastKind::FireAndForget, true), 0);
        assert_eq!(c.claim(1, hit(BALL, 2, 100.0, 0.0, 0.0), 500), CastCheck::Claimed { scale: 1.0 });
        assert_eq!(c.claim(1, hit(BALL, 3, 120.0, 0.0, 0.0), 550), CastCheck::Claimed { scale: 1.0 });
        // the same target again, and a target after the splash window
        assert_eq!(c.claim(1, hit(BALL, 3, 120.0, 0.0, 0.0), 560), CastCheck::NoCast);
        assert_eq!(c.claim(1, hit(BALL, 4, 130.0, 0.0, 0.0), 700), CastCheck::NoCast);
    }

    #[test]
    fn a_hit_beyond_the_spells_reach_is_refused() {
        let mut c = Casts::default();
        c.start(1, cast(BOLT, 1, CastKind::FireAndForget, false), 0);
        assert_eq!(c.claim(1, hit(BOLT, 2, 2_000.0, 0.0, 0.0), 50), CastCheck::TooFar);
        assert_eq!(c.claim(1, hit(BOLT, 2, 1_200.0, 0.0, 0.0), 60), CastCheck::Claimed { scale: 1.0 });
    }

    #[test]
    fn a_stream_counts_the_time_it_held_its_target_not_its_reports() {
        let mut c = Casts::default();
        c.start(1, cast(FLAMES, 1, CastKind::Concentration, false), 0);
        assert!(c.is_open(1, 1));
        // the first report after the stream's way out counts MAX_TICK_MS
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 600), CastCheck::Claimed { scale: 0.25 });
        // five reports a second count a fifth of a second each
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 800), CastCheck::Claimed { scale: 0.2 });
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 1_000), CastCheck::Claimed { scale: 0.2 });
        // ended: nothing more
        c.end(1, 1);
        assert!(!c.is_open(1, 1));
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 1_200), CastCheck::NoCast);
    }

    #[test]
    fn a_new_stream_in_a_hand_replaces_the_old_and_a_forgotten_caster_has_none() {
        let mut c = Casts::default();
        c.start(1, cast(FLAMES, 1, CastKind::Concentration, false), 0);
        c.start(1, cast(BOLT, 1, CastKind::Concentration, false), 100);
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 200), CastCheck::NoCast);
        assert_eq!(c.claim(1, hit(BOLT, 2, 150.0, 0.0, 0.0), 200), CastCheck::Claimed { scale: 0.1 });
        c.forget(1);
        assert_eq!(c.claim(1, hit(BOLT, 2, 150.0, 0.0, 0.0), 300), CastCheck::NoCast);
    }

    #[test]
    fn values_that_are_not_numbers_and_unknown_hands_record_nothing() {
        let mut c = Casts::default();
        let mut bad = cast(BOLT, 1, CastKind::FireAndForget, false);
        bad.x = f32::NAN;
        c.start(1, bad, 0);
        let mut far = cast(BOLT, 1, CastKind::FireAndForget, false);
        far.reach = f32::INFINITY;
        c.start(1, far, 0);
        c.start(1, cast(BOLT, 7, CastKind::Concentration, false), 0);
        assert_eq!(c.claim(1, hit(BOLT, 2, 0.0, 0.0, 0.0), 10), CastCheck::NoCast);
    }

    #[test]
    fn a_cast_lapses_after_its_window() {
        let mut c = Casts::default();
        c.start(1, cast(BOLT, 1, CastKind::FireAndForget, false), 0);
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), CAST_WINDOW_MS + 1), CastCheck::NoCast);
    }
}
