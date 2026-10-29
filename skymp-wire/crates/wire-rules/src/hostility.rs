//! Fights between players (thuum docs/verbs/hostility-sync.md, ADR-023). The
//! attacker's engine marks the victim an enemy by itself when its hit lands;
//! the victim's engine never sees that hit, so the server tells it when a
//! fight begins. Each game's figure of the other player then keeps fighting:
//! an AI in combat gives up only when it loses its target, and players stand
//! next to each other. So the server also ends a fight (ADR-023 amendment,
//! Eli 2026-10-05: "60 seconds OR walk apart"): when neither player has hit
//! the other for a minute, or when they have stood farther apart than the
//! engine's "enemies nearby" range for a few seconds.
//!
//! Since 2026-10-28 (thuum docs/verbs/npc-stream.md, H-53h) a hosted NPC is a
//! party too: a hit by or on an NPC a host's game simulates enters the table
//! when the other side is a player, since IsHostileToActor reads the table
//! for the effects between an effect's two ends (an atronach's cloak burns
//! the player it fights), and the host's engine fights for its NPC by itself,
//! so no notice goes to it. Two NPCs are their host's engine's business.

use std::collections::HashMap;

/// A fight ends when neither player has hit the other for this long.
pub const FIGHT_QUIET_MS: u64 = 60_000;

/// ... or when they have stood apart for this long.
pub const APART_MS: u64 = 5_000;

/// The engine's "enemies nearby" range outdoors, its refusal to wait or sleep
/// (fHostileActorExteriorDistance): the executable's default, which no master
/// file overrides (thuum ghidra/notes/hostility-1-7-104.md; lab/esm.py over
/// the five masters, 2026-10-05).
pub const EXTERIOR_RANGE: f32 = 3000.0;

/// The same indoors (fHostileActorInteriorDistance).
pub const INTERIOR_RANGE: f32 = 2000.0;

/// What the core knows about a hit it accepted.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HitFacts {
    /// A user plays the attacker.
    pub aggressor_is_player: bool,
    /// A host's game simulates the attacker: an NPC in a hosted cell that no
    /// user plays.
    pub aggressor_is_hosted: bool,
    /// A user plays the victim.
    pub target_is_player: bool,
    /// A host's game simulates the victim.
    pub target_is_hosted: bool,
    /// The attacker hit itself.
    pub same_actor: bool,
}

/// Whether a hit is one a fight here counts: a player on one side, and a
/// player or a hosted NPC on the other. An actor nobody plays or hosts is
/// nobody's to fight, and two NPCs are their host's engine's.
#[must_use]
pub fn counted(f: HitFacts) -> bool {
    let aggressor_named = f.aggressor_is_player || f.aggressor_is_hosted;
    let target_named = f.target_is_player || f.target_is_hosted;
    !f.same_actor && aggressor_named && target_named && (f.aggressor_is_player || f.target_is_player)
}

/// Whether two players stand apart: farther than the range where they are.
/// Players in different cells or worlds are apart; the core passes an
/// infinite distance for them, and a distance that is not a number counts as
/// apart too.
#[must_use]
pub fn apart(distance: f32, interior: bool) -> bool {
    let range = if interior { INTERIOR_RANGE } else { EXTERIOR_RANGE };
    distance.is_nan() || distance > range
}

#[derive(Debug, Clone, Copy)]
struct Fight {
    last_hit_ms: u64,
    apart_since_ms: Option<u64>,
}

/// The fights going on, by pair of actors: two players, or a player and a
/// hosted NPC. Times are a monotonic clock's milliseconds.
#[derive(Debug, Default)]
pub struct Fights {
    fights: HashMap<(u32, u32), Fight>,
}

const fn pair(a: u32, b: u32) -> (u32, u32) {
    if a <= b { (a, b) } else { (b, a) }
}

impl Fights {
    /// An accepted hit the rule counted: true when it begins a fight, so a
    /// victim a user plays is to be told; a hit in a fight going on keeps it
    /// going.
    pub fn hit(&mut self, aggressor: u32, victim: u32, now_ms: u64) -> bool {
        if aggressor == victim {
            return false;
        }
        let key = pair(aggressor, victim);
        if let Some(f) = self.fights.get_mut(&key) {
            f.last_hit_ms = now_ms;
            f.apart_since_ms = None;
            return false;
        }
        self.fights.insert(key, Fight { last_hit_ms: now_ms, apart_since_ms: None });
        true
    }

    /// The pairs fighting, lower actor id first, for the core to measure.
    #[must_use]
    pub fn pairs(&self) -> Vec<(u32, u32)> {
        let mut v: Vec<(u32, u32)> = self.fights.keys().copied().collect();
        v.sort_unstable();
        v
    }

    /// The core's measure of a pair now: true when its fight is over, which
    /// forgets it.
    pub fn check(&mut self, a: u32, b: u32, apart: bool, now_ms: u64) -> bool {
        let key = pair(a, b);
        let Some(f) = self.fights.get_mut(&key) else {
            return false;
        };
        let quiet = now_ms.saturating_sub(f.last_hit_ms) >= FIGHT_QUIET_MS;
        let walked_off = if apart {
            let since = *f.apart_since_ms.get_or_insert(now_ms);
            now_ms.saturating_sub(since) >= APART_MS
        } else {
            f.apart_since_ms = None;
            false
        };
        if quiet || walked_off {
            self.fights.remove(&key);
            return true;
        }
        false
    }

    /// Whether the actor is in a fight.
    #[must_use]
    pub fn in_fight(&self, actor: u32) -> bool {
        self.fights.keys().any(|&(x, y)| x == actor || y == actor)
    }

    /// Whether the two actors are in a fight with each other: what
    /// IsHostileToActor asks of an effect's victim and caster (thuum ADR-039,
    /// the vanilla cloaks' damage effects gate on it; a hosted creature's
    /// cloak since H-53h). Symmetric, as the record is.
    #[must_use]
    pub fn between(&self, a: u32, b: u32) -> bool {
        a != b && self.fights.contains_key(&pair(a, b))
    }

    /// The actor is gone: its fights end without a notice.
    pub fn forget(&mut self, actor: u32) {
        self.fights.retain(|&(x, y), _| x != actor && y != actor);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn players() -> HitFacts {
        HitFacts {
            aggressor_is_player: true,
            aggressor_is_hosted: false,
            target_is_player: true,
            target_is_hosted: false,
            same_actor: false,
        }
    }

    #[test]
    fn a_hit_counts_with_a_player_on_one_side_and_a_player_or_a_hosted_npc_on_the_other() {
        assert!(counted(players()));
        assert!(!counted(HitFacts { same_actor: true, ..players() }));
        // a hosted NPC's hit on a player begins a fight, and a player's on a hosted NPC
        assert!(counted(HitFacts { aggressor_is_player: false, aggressor_is_hosted: true, ..players() }));
        assert!(counted(HitFacts { target_is_player: false, target_is_hosted: true, ..players() }));
        // an actor nobody plays or hosts is nobody's to fight
        assert!(!counted(HitFacts { target_is_player: false, ..players() }));
        assert!(!counted(HitFacts { aggressor_is_player: false, ..players() }));
        // two NPCs never do, hosted or not: their host's engine's business
        let npcs = HitFacts {
            aggressor_is_player: false,
            aggressor_is_hosted: true,
            target_is_player: false,
            target_is_hosted: true,
            same_actor: false,
        };
        assert!(!counted(npcs));
        assert!(!counted(HitFacts { aggressor_is_hosted: false, target_is_hosted: false, ..npcs }));
    }

    #[test]
    fn the_first_hit_begins_a_fight_and_later_ones_keep_it() {
        let mut f = Fights::default();
        assert!(f.hit(1, 2, 0));
        assert!(!f.hit(2, 1, 3_000)); // the same fight, either way round
        assert!(!f.hit(1, 2, 50_000));
        assert!(f.in_fight(1) && f.in_fight(2) && !f.in_fight(3));
        assert!(f.between(1, 2) && f.between(2, 1));
        assert!(!f.between(1, 3) && !f.between(1, 1));
        assert_eq!(f.pairs(), vec![(1, 2)]);
        assert!(!f.hit(4, 4, 0)); // a self-hit is no fight
    }

    #[test]
    fn a_minute_without_a_hit_ends_it() {
        let mut f = Fights::default();
        f.hit(1, 2, 0);
        assert!(!f.check(1, 2, false, FIGHT_QUIET_MS - 1));
        f.hit(2, 1, 30_000); // a hit keeps it going
        assert!(!f.check(2, 1, false, FIGHT_QUIET_MS));
        assert!(f.check(1, 2, false, 30_000 + FIGHT_QUIET_MS));
        assert!(!f.in_fight(1));
        assert!(f.hit(1, 2, 100_000)); // the next hit begins a new one
    }

    #[test]
    fn walking_apart_for_a_few_seconds_ends_it() {
        let mut f = Fights::default();
        f.hit(1, 2, 0);
        assert!(!f.check(1, 2, true, 1_000)); // apart since 1 s
        assert!(!f.check(1, 2, true, 1_000 + APART_MS - 1));
        assert!(f.check(1, 2, true, 1_000 + APART_MS));
        // coming back together resets the count
        f.hit(1, 2, 10_000);
        assert!(!f.check(1, 2, true, 11_000));
        assert!(!f.check(1, 2, false, 14_000));
        assert!(!f.check(1, 2, true, 15_000));
        assert!(!f.check(1, 2, true, 15_000 + APART_MS - 1));
        // a hit while apart resets it too
        f.hit(2, 1, 19_000);
        assert!(!f.check(1, 2, true, 15_000 + APART_MS));
    }

    #[test]
    fn apart_means_beyond_the_engine_s_range() {
        assert!(!apart(EXTERIOR_RANGE, false));
        assert!(apart(EXTERIOR_RANGE + 1.0, false));
        assert!(apart(INTERIOR_RANGE + 1.0, true));
        assert!(!apart(INTERIOR_RANGE, true));
        assert!(apart(f32::INFINITY, false));
        assert!(apart(f32::NAN, false));
    }

    #[test]
    fn a_player_who_leaves_ends_its_fights() {
        let mut f = Fights::default();
        f.hit(1, 2, 0);
        f.hit(3, 1, 0);
        f.forget(1);
        assert!(f.pairs().is_empty());
    }
}
