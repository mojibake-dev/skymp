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

/// The most spells one cast's records reach (thuum docs/verbs/second-hand.md:
/// a cloak's spell, the spells its effects' scripts name).
pub const MAX_REACHED: usize = 8;

/// The engine's cadence for a cloak's applied spell, milliseconds: the
/// caster's game reports its hit on each target about every 500 ms (thuum
/// x-class-self 20261018-011427: Sotha's Maelstrom's and Blood Boil's procs
/// at 496 to 504 ms). A concentration reached spell's first report on a
/// target counts one cadence.
pub const REACHED_TICK_MS: u64 = 500;

/// The most one report of a concentration reached spell counts,
/// milliseconds: the cadence and a little, so the engine's own rhythm counts
/// in full (a stream's MAX_TICK_MS would count half of it) and a longer gap
/// never counts time the proc did not hold its target.
pub const REACHED_MAX_TICK_MS: u64 = 600;

/// How often a fire-and-forget reached spell may claim a hit on one target,
/// milliseconds: under the cadence, so a proc the engine applies each
/// cadence is never refused, and a report faster than the engine's is.
pub const REACHED_MIN_GAP_MS: u64 = 400;

/// The targets one cast's reached spells keep a rate for; past this many,
/// the oldest is forgotten.
pub const MAX_REACHED_TARGETS: usize = 8;

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
    /// The spells the cast's records reach, 0 for an empty slot (thuum
    /// docs/verbs/second-hand.md): a hit by one of them claims this cast
    /// through [`Casts::claim_reached`]. Read from the records by the caller.
    pub reached: [u32; MAX_REACHED],
    /// How long a reached spell may claim this cast, milliseconds: its
    /// longest effect's duration (at least CAST_WINDOW_MS, and as long as a
    /// concentration cast's stream stays open).
    pub linger_ms: u64,
    /// Bit i set: `reached[i]` is a concentration spell (a cloak's applied
    /// spell is one), whose hits count the time they held their target, as
    /// a stream's do; clear: a fire-and-forget proc, once per
    /// REACHED_TICK_MS.
    pub reached_concentration: u8,
    /// Every effect of the spell is delivered to its target, by touch or at
    /// a target actor (MGEF delivery 1 or 3), so no aim decides where it
    /// lands and the caster's game names its target only by a guess (thuum
    /// docs/verbs/spell-cast.md, playtest twenty-three: every Shadowbond
    /// and Azure Reconstruction cast went out with target 0 while their
    /// hits named the player). Such a cast's relay to the other games waits
    /// for its first hit, which names the target the server trusts; a cast
    /// that hits nobody is never relayed.
    pub lands_on_target: bool,
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
    /// concentration hit, whose magnitude is per second). `relay`: the hit
    /// is the first of a cast whose relay waited for it
    /// (`CastFacts::lands_on_target`), so the cast goes to the other games
    /// now, with this hit's target.
    Claimed {
        /// The factor on the spell's magnitude.
        scale: f32,
        /// Relay the cast now, with this hit's target.
        relay: bool,
    },
    /// No cast of that spell covers the hit.
    NoCast,
    /// Casts of that spell wait, but the target is beyond their reach.
    TooFar,
    /// A reached spell's hit on a target that one claimed within
    /// REACHED_TICK_MS.
    Throttled,
}

#[derive(Debug, Clone, Copy, PartialEq)]
struct Released {
    facts: CastFacts,
    at_ms: u64,
    /// The cast's relay waits for its first hit (lands_on_target) and has
    /// not gone yet.
    relay_pending: bool,
    /// The first hit's time and every target it covered.
    first_hit_ms: Option<u64>,
    targets: [u32; MAX_SPLASH_TARGETS],
    hits: usize,
}

#[derive(Debug, Clone, Copy, PartialEq)]
struct Open {
    facts: CastFacts,
    last_counted_ms: u64,
    /// As `Released::relay_pending`.
    relay_pending: bool,
}

/// A cast whose records reach other spells, kept for its linger.
#[derive(Debug, Clone, Copy, PartialEq)]
struct Lingering {
    facts: CastFacts,
    at_ms: u64,
    /// The last claim on each target, (target, ms); 0 for an empty slot.
    claims: [(u32, u64); MAX_REACHED_TARGETS],
}

#[derive(Debug, Default)]
struct ActorCasts {
    released: VecDeque<Released>,
    open: [Option<Open>; 4],
    lingering: VecDeque<Lingering>,
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

/// Whether a cast's reached spells may still claim it at `now_ms`: within
/// its linger (at least CAST_WINDOW_MS), or while its concentration stream
/// stays open in its hand.
fn lingers(l: &Lingering, open: &[Option<Open>; 4], now_ms: u64) -> bool {
    if now_ms.saturating_sub(l.at_ms) <= l.facts.linger_ms.max(CAST_WINDOW_MS) {
        return true;
    }
    l.facts.kind == CastKind::Concentration
        && open
            .get(usize::from(l.facts.hand))
            .and_then(Option::as_ref)
            .is_some_and(|o| o.facts.spell == l.facts.spell)
}

impl Casts {
    /// `actor` began a cast at `now_ms` (a monotonic clock): a
    /// fire-and-forget one waits for its hit, a concentration one opens in
    /// its hand, ending any stream that hand held. A cast whose position or
    /// reach is not a number, or from a hand outside 0 to 3, is not recorded,
    /// so its hit is refused. Returns whether the cast's relay to the other
    /// games waits for its first hit (`CastFacts::lands_on_target`): true,
    /// the caller holds the relay until a [`CastCheck::Claimed`] with
    /// `relay` set names the target; false, it relays at once.
    pub fn start(&mut self, actor: u32, facts: CastFacts, now_ms: u64) -> bool {
        let hand = usize::from(facts.hand);
        if hand >= 4 || !finite(&facts) {
            return false;
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
                    relay_pending: facts.lands_on_target,
                    first_hit_ms: None,
                    targets: [0; MAX_SPLASH_TARGETS],
                    hits: 0,
                });
            }
            CastKind::Concentration => {
                if let Some(slot) = casts.open.get_mut(hand) {
                    *slot = Some(Open { facts, last_counted_ms: now_ms, relay_pending: facts.lands_on_target });
                }
            }
        }
        // thuum docs/verbs/second-hand.md: a cast whose records reach other
        // spells waits for their hits for its linger
        if facts.reached.iter().any(|s| *s != 0) {
            let open = &casts.open;
            casts.lingering.retain(|l| lingers(l, open, now_ms));
            if casts.lingering.len() >= MAX_CASTS {
                casts.lingering.pop_front();
            }
            casts.lingering.push_back(Lingering { facts, at_ms: now_ms, claims: [(0, 0); MAX_REACHED_TARGETS] });
        }
        facts.lands_on_target
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
    /// counts the time since its last counted hit, at most MAX_TICK_MS (with
    /// a stream of that spell in each hand, the one whose last counted hit is
    /// oldest: the game runs each hand's stream as an effect of its own on
    /// the target, both working, and its hit reports do not say which hand,
    /// thuum docs/verbs/spell-cast.md, x-dual-stack); otherwise the newest
    /// fire-and-forget cast of that spell that can still take a hit (unused,
    /// or an area cast's further target within SPLASH_MS of its first) and
    /// whose reach covers the target, once per target.
    pub fn claim(&mut self, actor: u32, hit: CastHit, now_ms: u64) -> CastCheck {
        let CastHit { spell, target, x, y, z } = hit;
        let Some(casts) = self.per_actor.get_mut(&actor) else {
            return CastCheck::NoCast;
        };
        if !(x.is_finite() && y.is_finite() && z.is_finite()) {
            return CastCheck::NoCast;
        }
        let mut any = false;
        let mut oldest: Option<(usize, u64)> = None;
        for (hand, open) in casts.open.iter().enumerate() {
            let Some(open) = open else {
                continue;
            };
            if open.facts.spell != spell {
                continue;
            }
            any = true;
            if within(&open.facts, x, y, z) && oldest.is_none_or(|(_, at)| open.last_counted_ms < at) {
                oldest = Some((hand, open.last_counted_ms));
            }
        }
        if let Some(open) = oldest.and_then(|(hand, _)| casts.open.get_mut(hand)).and_then(Option::as_mut) {
            let tick_ms = now_ms.saturating_sub(open.last_counted_ms).min(MAX_TICK_MS);
            open.last_counted_ms = now_ms;
            let scale = f32::from(u16::try_from(tick_ms).unwrap_or(u16::MAX)) / 1000.0;
            let relay = std::mem::replace(&mut open.relay_pending, false);
            return CastCheck::Claimed { scale, relay };
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
            let relay = std::mem::replace(&mut cast.relay_pending, false);
            return CastCheck::Claimed { scale: 1.0, relay };
        }
        if any {
            CastCheck::TooFar
        } else {
            CastCheck::NoCast
        }
    }

    /// A hit by `actor` at `now_ms` with a spell none of its casts is of
    /// (thuum docs/verbs/second-hand.md): the newest cast whose records
    /// reach the hit's spell and still lingers covers it, when the target
    /// stands within that cast's reach of where the server has the caster
    /// now (a cloak moves with its bearer). A concentration reached spell
    /// (a cloak's damage) counts the time since its last counted hit on that
    /// target, at most REACHED_MAX_TICK_MS (the first counts one
    /// REACHED_TICK_MS), as a stream does; a fire-and-forget one counts its
    /// magnitude once, at most once per target each REACHED_MIN_GAP_MS.
    pub fn claim_reached(&mut self, actor: u32, hit: CastHit, caster: [f32; 3], now_ms: u64) -> CastCheck {
        let CastHit { spell, target, x, y, z } = hit;
        let [cx, cy, cz] = caster;
        if spell == 0 || target == 0 || ![x, y, z, cx, cy, cz].iter().all(|v| v.is_finite()) {
            return CastCheck::NoCast;
        }
        let Some(casts) = self.per_actor.get_mut(&actor) else {
            return CastCheck::NoCast;
        };
        let open = &casts.open;
        casts.lingering.retain(|l| lingers(l, open, now_ms));
        let mut any = false;
        for l in casts.lingering.iter_mut().rev() {
            let Some(index) = l.facts.reached.iter().position(|r| *r == spell) else {
                continue;
            };
            any = true;
            let (dx, dy, dz) = (x - cx, y - cy, z - cz);
            if (dx * dx + dy * dy + dz * dz).sqrt() > l.facts.reach.max(0.0) + RANGE_SLACK {
                continue;
            }
            let concentration = u32::try_from(index).ok().and_then(|i| l.facts.reached_concentration.checked_shr(i)).is_some_and(|bits| bits & 1 == 1);
            let last = l.claims.iter().find(|(t, _)| *t == target).map(|(_, last)| *last);
            let tick_ms = match last {
                Some(last) => now_ms.saturating_sub(last),
                None => REACHED_TICK_MS,
            };
            if !concentration && last.is_some() && tick_ms < REACHED_MIN_GAP_MS {
                return CastCheck::Throttled;
            }
            if let Some(slot) = l.claims.iter_mut().find(|(t, _)| *t == target) {
                slot.1 = now_ms;
            } else if let Some(slot) = l.claims.iter_mut().min_by_key(|(t, last)| (*t != 0, *last)) {
                *slot = (target, now_ms);
            }
            let scale = if concentration {
                f32::from(u16::try_from(tick_ms.min(REACHED_MAX_TICK_MS)).unwrap_or(u16::MAX)) / 1000.0
            } else {
                1.0
            };
            return CastCheck::Claimed { scale, relay: false };
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
        CastFacts { spell, hand, kind, area, reach: 1_000.0, x: 0.0, y: 0.0, z: 0.0, reached: [0; MAX_REACHED], linger_ms: 0, reached_concentration: 0, lands_on_target: false }
    }

    // labels, not records: a cloak spell and its proc, a bolt and its proc
    const CLOAK: u32 = 0xa101;
    const CLOAK_PROC: u32 = 0xa102;
    const ZAP: u32 = 0xa103;
    const ZAP_PROC: u32 = 0xa104;

    // a cast whose records reach `proc`, lingering `linger_ms`
    fn reaching(spell: u32, kind: CastKind, proc: u32, linger_ms: u64) -> CastFacts {
        let mut reached = [0; MAX_REACHED];
        reached[0] = proc;
        CastFacts { reached, linger_ms, ..cast(spell, 1, kind, false) }
    }

    // a spell delivered to its target: its relay waits for its first hit
    fn landing(spell: u32, kind: CastKind) -> CastFacts {
        CastFacts { lands_on_target: true, ..cast(spell, 1, kind, false) }
    }

    #[test]
    fn a_landing_casts_relay_waits_for_its_first_hit_and_goes_once() {
        let mut c = Casts::default();
        assert!(c.start(1, landing(BOLT, CastKind::FireAndForget), 0));
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), 50), CastCheck::Claimed { scale: 1.0, relay: true });
        // an area landing cast's further target relays nothing more
        let mut c = Casts::default();
        assert!(c.start(1, CastFacts { area: true, ..landing(BALL, CastKind::FireAndForget) }, 0));
        assert_eq!(c.claim(1, hit(BALL, 2, 100.0, 0.0, 0.0), 50), CastCheck::Claimed { scale: 1.0, relay: true });
        assert_eq!(c.claim(1, hit(BALL, 3, 120.0, 0.0, 0.0), 80), CastCheck::Claimed { scale: 1.0, relay: false });
    }

    #[test]
    fn a_landing_streams_first_hit_relays_and_its_later_hits_do_not() {
        let mut c = Casts::default();
        assert!(c.start(1, landing(FLAMES, CastKind::Concentration), 0));
        assert_eq!(c.claim(1, hit(FLAMES, 2, 100.0, 0.0, 0.0), 200), CastCheck::Claimed { scale: 0.2, relay: true });
        assert_eq!(c.claim(1, hit(FLAMES, 2, 100.0, 0.0, 0.0), 400), CastCheck::Claimed { scale: 0.2, relay: false });
        // a new stream in the same hand waits again
        assert!(c.start(1, landing(FLAMES, CastKind::Concentration), 1_000));
        assert_eq!(c.claim(1, hit(FLAMES, 2, 100.0, 0.0, 0.0), 1_100), CastCheck::Claimed { scale: 0.1, relay: true });
    }

    #[test]
    fn a_cast_that_is_not_delivered_to_its_target_relays_at_once() {
        let mut c = Casts::default();
        assert!(!c.start(1, cast(BOLT, 1, CastKind::FireAndForget, false), 0));
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), 50), CastCheck::Claimed { scale: 1.0, relay: false });
        // an unrecorded cast (a bad hand) holds nothing
        assert!(!c.start(1, CastFacts { hand: 7, ..landing(BOLT, CastKind::FireAndForget) }, 0));
    }

    #[test]
    fn a_landing_cast_that_hits_nobody_within_the_window_is_never_relayed() {
        let mut c = Casts::default();
        assert!(c.start(1, landing(BOLT, CastKind::FireAndForget), 0));
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), CAST_WINDOW_MS + 1), CastCheck::NoCast);
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
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), 50), CastCheck::Claimed { scale: 1.0, relay: false });
        assert_eq!(c.claim(1, hit(BOLT, 2, 100.0, 0.0, 0.0), 60), CastCheck::NoCast);
    }

    #[test]
    fn an_area_cast_covers_its_other_targets_once_each_within_the_splash() {
        let mut c = Casts::default();
        c.start(1, cast(BALL, 1, CastKind::FireAndForget, true), 0);
        assert_eq!(c.claim(1, hit(BALL, 2, 100.0, 0.0, 0.0), 500), CastCheck::Claimed { scale: 1.0, relay: false });
        assert_eq!(c.claim(1, hit(BALL, 3, 120.0, 0.0, 0.0), 550), CastCheck::Claimed { scale: 1.0, relay: false });
        // the same target again, and a target after the splash window
        assert_eq!(c.claim(1, hit(BALL, 3, 120.0, 0.0, 0.0), 560), CastCheck::NoCast);
        assert_eq!(c.claim(1, hit(BALL, 4, 130.0, 0.0, 0.0), 700), CastCheck::NoCast);
    }

    #[test]
    fn a_hit_beyond_the_spells_reach_is_refused() {
        let mut c = Casts::default();
        c.start(1, cast(BOLT, 1, CastKind::FireAndForget, false), 0);
        assert_eq!(c.claim(1, hit(BOLT, 2, 2_000.0, 0.0, 0.0), 50), CastCheck::TooFar);
        assert_eq!(c.claim(1, hit(BOLT, 2, 1_200.0, 0.0, 0.0), 60), CastCheck::Claimed { scale: 1.0, relay: false });
    }

    #[test]
    fn a_stream_counts_the_time_it_held_its_target_not_its_reports() {
        let mut c = Casts::default();
        c.start(1, cast(FLAMES, 1, CastKind::Concentration, false), 0);
        assert!(c.is_open(1, 1));
        // the first report after the stream's way out counts MAX_TICK_MS
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 600), CastCheck::Claimed { scale: 0.25, relay: false });
        // five reports a second count a fifth of a second each
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 800), CastCheck::Claimed { scale: 0.2, relay: false });
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 1_000), CastCheck::Claimed { scale: 0.2, relay: false });
        // ended: nothing more
        c.end(1, 1);
        assert!(!c.is_open(1, 1));
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 1_200), CastCheck::NoCast);
    }

    #[test]
    fn two_hands_of_one_stream_count_each_its_own_time() {
        // x-dual-stack: the game runs each hand's Flames as an effect of its
        // own on the target; the two hands' reports come every 200 ms each,
        // the second's 50 ms behind the first's
        let mut c = Casts::default();
        c.start(1, cast(FLAMES, 1, CastKind::Concentration, false), 0);
        c.start(1, cast(FLAMES, 0, CastKind::Concentration, false), 50);
        let mut counted = 0.0_f32;
        for t in (200..=1_000).step_by(200) {
            for at in [t, t + 50] {
                let check = c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), at);
                assert!(matches!(check, CastCheck::Claimed { .. }), "{check:?} at {at}");
                if let CastCheck::Claimed { scale, .. } = check {
                    counted += scale;
                }
            }
        }
        // two streams from 0 and 50 to 1000 and 1050: two seconds
        assert!((counted - 2.0).abs() < 1e-3, "{counted}");
    }

    #[test]
    fn one_hand_missing_its_target_counts_at_most_a_tick_more() {
        // both hands stream Flames but only one series of reports comes in:
        // each report takes the hand that waited longest, and a hand counts
        // at most MAX_TICK_MS, so the two halves count a quarter more than
        // the one stream's time, not twice it
        let mut c = Casts::default();
        c.start(1, cast(FLAMES, 1, CastKind::Concentration, false), 0);
        c.start(1, cast(FLAMES, 0, CastKind::Concentration, false), 50);
        let mut counted = 0.0_f32;
        for t in (200..=2_000).step_by(200) {
            if let CastCheck::Claimed { scale, .. } = c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), t) {
                counted += scale;
            }
        }
        assert!(counted <= 2.0 * 1.25 + 1e-3, "{counted}");
        assert!(counted >= 2.0 - 1e-3, "{counted}");
    }

    #[test]
    fn a_new_stream_in_a_hand_replaces_the_old_and_a_forgotten_caster_has_none() {
        let mut c = Casts::default();
        c.start(1, cast(FLAMES, 1, CastKind::Concentration, false), 0);
        c.start(1, cast(BOLT, 1, CastKind::Concentration, false), 100);
        assert_eq!(c.claim(1, hit(FLAMES, 2, 150.0, 0.0, 0.0), 200), CastCheck::NoCast);
        assert_eq!(c.claim(1, hit(BOLT, 2, 150.0, 0.0, 0.0), 200), CastCheck::Claimed { scale: 0.1, relay: false });
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

    #[test]
    fn a_fire_and_forget_proc_claims_its_cast_for_the_cloaks_seconds_at_the_engines_rhythm() {
        let mut c = Casts::default();
        c.start(1, reaching(CLOAK, CastKind::FireAndForget, CLOAK_PROC, 60_000), 0);
        let at = [0.0, 0.0, 0.0];
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), at, 1_000), CastCheck::Claimed { scale: 1.0, relay: false });
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), at, 1_200), CastCheck::Throttled);
        // another target has its own rate
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 3, 0.0, 300.0, 0.0), at, 1_200), CastCheck::Claimed { scale: 1.0, relay: false });
        // the engine's cadence, 496 ms in x-class-self, is never refused
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), at, 1_496), CastCheck::Claimed { scale: 1.0, relay: false });
        // past the CAST_WINDOW_MS a bolt has, still within the cloak's
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), at, 40_000), CastCheck::Claimed { scale: 1.0, relay: false });
        // and not after it
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), at, 60_001), CastCheck::NoCast);
    }

    #[test]
    fn a_cloaks_concentration_spell_counts_the_time_it_held_each_target() {
        let mut c = Casts::default();
        // bit 0: the reached spell is a concentration spell (a cloak's damage)
        c.start(1, CastFacts { reached_concentration: 1, ..reaching(CLOAK, CastKind::FireAndForget, CLOAK_PROC, 60_000) }, 0);
        let on = |t| hit(CLOAK_PROC, t, 300.0, 0.0, 0.0);
        // the first report on a target counts one cadence, then each the time
        // since the last, the engine's 500 ms rhythm in full
        assert_eq!(c.claim_reached(1, on(2), [0.0; 3], 1_000), CastCheck::Claimed { scale: 0.5, relay: false });
        assert_eq!(c.claim_reached(1, on(2), [0.0; 3], 1_499), CastCheck::Claimed { scale: 0.499, relay: false });
        // reports faster than the engine's are never throttled, and add up to the time
        assert_eq!(c.claim_reached(1, on(2), [0.0; 3], 1_499), CastCheck::Claimed { scale: 0.0, relay: false });
        // a gap counts at most REACHED_MAX_TICK_MS
        assert_eq!(c.claim_reached(1, on(2), [0.0; 3], 5_000), CastCheck::Claimed { scale: 0.6, relay: false });
        // another target's time is its own
        assert_eq!(c.claim_reached(1, on(3), [0.0; 3], 5_000), CastCheck::Claimed { scale: 0.5, relay: false });
    }

    #[test]
    fn a_proc_is_judged_from_where_the_caster_stands_now() {
        let mut c = Casts::default();
        c.start(1, reaching(CLOAK, CastKind::FireAndForget, CLOAK_PROC, 60_000), 0);
        // the bearer walked 5000 units: a target beside it is within reach
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 2, 5_100.0, 0.0, 0.0), [5_000.0, 0.0, 0.0], 1_000), CastCheck::Claimed { scale: 1.0, relay: false });
        // one by the cast's spot is not
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 3, 0.0, 0.0, 0.0), [5_000.0, 0.0, 0.0], 1_000), CastCheck::TooFar);
    }

    #[test]
    fn a_spell_no_cast_reaches_is_refused_and_the_own_claim_is_unchanged() {
        let mut c = Casts::default();
        c.start(1, reaching(CLOAK, CastKind::FireAndForget, CLOAK_PROC, 60_000), 0);
        assert_eq!(c.claim_reached(1, hit(ZAP_PROC, 2, 300.0, 0.0, 0.0), [0.0; 3], 1_000), CastCheck::NoCast);
        assert_eq!(c.claim_reached(7, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), [0.0; 3], 1_000), CastCheck::NoCast);
        // a proc's hit is not a hit of the cast's own spell
        assert_eq!(c.claim(1, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), 1_000), CastCheck::NoCast);
        // and the cast's own hit still claims as before
        assert_eq!(c.claim(1, hit(CLOAK, 2, 300.0, 0.0, 0.0), 1_000), CastCheck::Claimed { scale: 1.0, relay: false });
    }

    #[test]
    fn a_bolts_proc_claims_within_the_cast_window() {
        let mut c = Casts::default();
        c.start(1, reaching(ZAP, CastKind::FireAndForget, ZAP_PROC, 0), 0);
        assert_eq!(c.claim_reached(1, hit(ZAP_PROC, 2, 600.0, 0.0, 0.0), [0.0; 3], 10_000), CastCheck::Claimed { scale: 1.0, relay: false });
        assert_eq!(c.claim_reached(1, hit(ZAP_PROC, 2, 600.0, 0.0, 0.0), [0.0; 3], CAST_WINDOW_MS + 1), CastCheck::NoCast);
    }

    #[test]
    fn a_concentration_cloak_lingers_while_its_stream_is_open() {
        let mut c = Casts::default();
        c.start(1, reaching(CLOAK, CastKind::Concentration, CLOAK_PROC, 0), 0);
        let late = CAST_WINDOW_MS + 5_000;
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), [0.0; 3], late), CastCheck::Claimed { scale: 1.0, relay: false });
        c.end(1, 1);
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), [0.0; 3], late + 1_000), CastCheck::NoCast);
    }

    #[test]
    fn a_reached_claim_with_values_that_are_not_numbers_or_no_target_is_refused() {
        let mut c = Casts::default();
        c.start(1, reaching(CLOAK, CastKind::FireAndForget, CLOAK_PROC, 60_000), 0);
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 2, 300.0, 0.0, 0.0), [f32::NAN, 0.0, 0.0], 1_000), CastCheck::NoCast);
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, 0, 300.0, 0.0, 0.0), [0.0; 3], 1_000), CastCheck::NoCast);
        assert_eq!(c.claim_reached(1, hit(0, 2, 300.0, 0.0, 0.0), [0.0; 3], 1_000), CastCheck::NoCast);
    }

    #[test]
    fn many_targets_keep_the_newest_rates() {
        let mut c = Casts::default();
        c.start(1, reaching(CLOAK, CastKind::FireAndForget, CLOAK_PROC, 60_000), 0);
        let kept = u32::try_from(MAX_REACHED_TARGETS).unwrap_or(u32::MAX);
        for t in 2..(2 + kept + 3) {
            assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, t, 300.0, 0.0, 0.0), [0.0; 3], 1_000), CastCheck::Claimed { scale: 1.0, relay: false });
        }
        // the newest target's rate is kept
        let newest = 2 + kept + 2;
        assert_eq!(c.claim_reached(1, hit(CLOAK_PROC, newest, 300.0, 0.0, 0.0), [0.0; 3], 1_100), CastCheck::Throttled);
    }
}
