//! The game rules for the C++ core (thuum ADR-020): the core gathers the
//! facts from its world model and asks; wire-rules decides. Plain values both
//! ways, and the movement budgets live here.

use wire_rules::{activation, appearance, damage, melee, movement};

#[cxx::bridge(namespace = "skymp::rules")]
mod ffi {
    /// A decision and the bound it was made against (units), for the log.
    #[derive(Debug)]
    struct Verdict {
        /// The rule lets the action through.
        allowed: bool,
        /// The limit, units.
        bound: f32,
    }

    /// What the core knows about the race a client chose in the race menu.
    #[derive(Debug)]
    struct RaceFacts {
        /// The core has game files to judge by.
        has_game_files: bool,
        /// The race is the one the core records for the actor.
        is_recorded_race: bool,
        /// The form is a RACE with the Playable flag.
        is_playable_race: bool,
    }

    /// What the core knows about a player's melee hit on another player.
    #[derive(Debug)]
    struct MeleeFacts {
        /// Center to center, units.
        distance: f32,
        /// fCombatDistance.
        combat_distance: f32,
        /// fCombatBashReach.
        bash_reach: f32,
        /// The longest DNAM reach among the worn weapons, 0 for none.
        weapon_reach: f32,
        /// The attacker race's unarmed reach.
        unarmed_reach: f32,
        /// The attacker's race height for its sex.
        aggressor_scale: f32,
        /// The target's race height for its sex.
        target_scale: f32,
    }

    /// What the core knows about a player's hit flags.
    #[derive(Debug)]
    struct FlagFacts {
        /// The client says power attack.
        claims_power: bool,
        /// The client says sneak attack.
        claims_sneak: bool,
        /// Whether the attacker ever started a power attack.
        saw_power_start: bool,
        /// Milliseconds since that start (0 when there was none).
        since_power_start_ms: u64,
        /// The core holds the attacker sneaking.
        is_sneaking: bool,
    }

    /// The hit flags the server keeps.
    #[derive(Debug)]
    struct Flags {
        /// Power attack kept.
        power: bool,
        /// Sneak attack kept.
        sneak: bool,
    }

    extern "Rust" {
        /// A client's first activation of a target `distance` units away,
        /// `target_size` being its bounds' farthest point times its scale.
        fn activation_within_reach(distance: f32, target_size: f32) -> Verdict;
        /// Whether the race menu's result may set the race.
        fn race_allowed(facts: &RaceFacts) -> bool;
        /// Whether a player's melee hit on a player is within reach.
        fn melee_within_reach(facts: &MeleeFacts) -> Verdict;
        /// The hit flags the server keeps.
        fn backed_flags(facts: &FlagFacts) -> Flags;

        /// Every player actor's ground speed budget.
        type MovementBudgets;
        /// Empty budgets: each actor starts full.
        fn new_movement_budgets() -> Box<MovementBudgets>;
        /// Charge `actor` a move of `ground` units at `now_ms` (a monotonic
        /// clock); true when it fits.
        fn spend(self: &mut MovementBudgets, actor: u32, ground: f32, now_ms: u64) -> bool;
        /// Drop an actor's budget.
        fn forget(self: &mut MovementBudgets, actor: u32);
    }
}

pub use ffi::{FlagFacts, Flags, MeleeFacts, RaceFacts, Verdict};

fn verdict(v: wire_rules::Verdict) -> Verdict {
    Verdict { allowed: v.allowed, bound: v.bound }
}

fn activation_within_reach(distance: f32, target_size: f32) -> Verdict {
    verdict(activation::within_reach(distance, target_size))
}

fn race_allowed(f: &RaceFacts) -> bool {
    appearance::race_allowed(appearance::RaceFacts {
        has_game_files: f.has_game_files,
        is_recorded_race: f.is_recorded_race,
        is_playable_race: f.is_playable_race,
    })
}

fn melee_within_reach(f: &MeleeFacts) -> Verdict {
    verdict(melee::within_reach(melee::MeleeFacts {
        distance: f.distance,
        combat_distance: f.combat_distance,
        bash_reach: f.bash_reach,
        weapon_reach: f.weapon_reach,
        unarmed_reach: f.unarmed_reach,
        aggressor_scale: f.aggressor_scale,
        target_scale: f.target_scale,
    }))
}

fn backed_flags(f: &FlagFacts) -> Flags {
    let kept = damage::backed_flags(damage::FlagFacts {
        claims_power: f.claims_power,
        claims_sneak: f.claims_sneak,
        since_power_start_ms: f.saw_power_start.then_some(f.since_power_start_ms),
        is_sneaking: f.is_sneaking,
    });
    Flags { power: kept.power, sneak: kept.sneak }
}

/// Every player actor's ground speed budget (wire-rules movement).
#[derive(Debug, Default)]
pub struct MovementBudgets(movement::Budgets);

fn new_movement_budgets() -> Box<MovementBudgets> {
    Box::default()
}

impl MovementBudgets {
    fn spend(&mut self, actor: u32, ground: f32, now_ms: u64) -> bool {
        self.0.spend(actor, ground, now_ms)
    }

    fn forget(&mut self, actor: u32) {
        self.0.forget(actor);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_bridge_passes_the_rules_through() {
        assert!(!activation_within_reach(1509.0, 0.0).allowed);
        assert!(race_allowed(&RaceFacts { has_game_files: true, is_recorded_race: false, is_playable_race: true }));
        let v = melee_within_reach(&MeleeFacts {
            distance: 500.0,
            combat_distance: 141.0,
            bash_reach: 141.0,
            weapon_reach: 1.0,
            unarmed_reach: 96.0,
            aggressor_scale: 1.03,
            target_scale: 1.03,
        });
        assert!(!v.allowed);
        let kept = backed_flags(&FlagFacts {
            claims_power: true,
            claims_sneak: true,
            saw_power_start: false,
            since_power_start_ms: 0,
            is_sneaking: true,
        });
        assert!(!kept.power && kept.sneak);
        let mut b = new_movement_budgets();
        assert!(b.spend(7, 2048.0, 0));
        assert!(!b.spend(7, 1.0, 0));
    }
}
