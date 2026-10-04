//! Whether an accepted hit tells the victim's game that it is in a fight
//! (thuum docs/verbs/hostility-sync.md, ADR-023). The attacker's engine marks
//! the victim an enemy by itself when its hit lands; the victim's engine never
//! sees that hit, so the server tells it. Once a fight: on the first hit, and
//! again after a pause as long as the rest rule's quiet window, so the two
//! verbs share one notion of fighting.

use crate::rest::COMBAT_QUIET_MS;

/// What the core knows about a hit it accepted.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HitFacts {
    /// A user plays the attacker.
    pub aggressor_is_player: bool,
    /// A user plays the victim.
    pub target_is_player: bool,
    /// The attacker hit itself.
    pub same_actor: bool,
    /// Milliseconds since the attacker's previous hit on this victim, if any.
    pub since_previous_hit_ms: Option<u64>,
}

/// Whether the victim's game is to be told that the attacker is hostile.
#[must_use]
pub fn notify_victim(f: HitFacts) -> bool {
    f.aggressor_is_player
        && f.target_is_player
        && !f.same_actor
        && f.since_previous_hit_ms.is_none_or(|ms| ms >= COMBAT_QUIET_MS)
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn between_players(since: Option<u64>) -> HitFacts {
        HitFacts { aggressor_is_player: true, target_is_player: true, same_actor: false, since_previous_hit_ms: since }
    }

    #[test]
    fn the_first_hit_of_a_fight_tells_the_victim() {
        assert!(notify_victim(between_players(None)));
    }

    #[test]
    fn hits_under_the_quiet_window_apart_are_one_fight() {
        assert!(!notify_victim(between_players(Some(0))));
        assert!(!notify_victim(between_players(Some(3_000))));
        assert!(!notify_victim(between_players(Some(COMBAT_QUIET_MS - 1))));
    }

    #[test]
    fn a_hit_after_the_quiet_window_starts_a_new_fight() {
        assert!(notify_victim(between_players(Some(COMBAT_QUIET_MS))));
        assert!(notify_victim(between_players(Some(60_000))));
    }

    #[test]
    fn only_a_hit_between_two_players_tells_anyone() {
        let npc_victim = HitFacts { target_is_player: false, ..between_players(None) };
        let npc_attacker = HitFacts { aggressor_is_player: false, ..between_players(None) };
        let self_hit = HitFacts { same_actor: true, ..between_players(None) };
        assert!(!notify_victim(npc_victim));
        assert!(!notify_victim(npc_attacker));
        assert!(!notify_victim(self_hit));
    }
}
