//! The game rules for the C++ core (thuum ADR-020): the core gathers the
//! facts from its world model and asks; wire-rules decides. Plain values both
//! ways; the movement budgets and the game clock (ADR-021) live here.

use wire_rules::{activation, appearance, clock, damage, melee, movement, rest};

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

    /// What the core knows about the direction of a player's melee hit on a
    /// player (thuum docs/verbs/hit-cone.md).
    #[derive(Debug)]
    struct ConeFacts {
        /// The attacker's heading as the core records it, degrees.
        heading: f32,
        /// Target minus attacker, east, units.
        dx: f32,
        /// Target minus attacker, north, units.
        dy: f32,
        /// The widest ATKD strike angle of the attacker's race, 0 for none.
        widest_strike_angle: f32,
        /// The hit is a power attack, as the core kept the flag.
        power: bool,
        /// The target is dead.
        target_dead: bool,
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

    /// The game clock at one instant, as the engine's six time globals hold
    /// it (thuum docs/verbs/time.md).
    #[derive(Debug)]
    struct GameTime {
        /// GameYear.
        year: u32,
        /// GameMonth, from 0.
        month: u32,
        /// GameDay, from 1.
        day: u32,
        /// GameHour, at least 0 and below 24.
        hour: f32,
        /// GameDaysPassed.
        days_passed: f32,
        /// TimeScale.
        time_scale: f32,
    }

    /// What the core knows about a player's rest (thuum docs/verbs/rest.md).
    #[derive(Debug)]
    struct RestFacts {
        /// The game hours the client says its player rested.
        hours: f32,
        /// A sleep in a bed rather than a wait.
        sleep: bool,
        /// The core holds the player dead.
        is_dead: bool,
        /// The player dealt or took a hit the core saw.
        has_hit: bool,
        /// Milliseconds since the last such hit (0 when there was none).
        since_last_hit_ms: u64,
    }

    /// Why a rest is refused, `Allowed` when it is let through.
    #[derive(Debug)]
    enum RestRefusal {
        /// Let through.
        Allowed,
        /// Hours outside the menu's 1 to 24, or not a number.
        Hours,
        /// The server switched this kind of rest off.
        Off,
        /// The player is dead.
        Dead,
        /// A hit within the last 10 s.
        Fighting,
    }

    /// One attribute as the core's CropRegeneration takes it.
    #[derive(Debug)]
    struct Regen {
        /// The current percentage, 0 to 1.
        percentage: f32,
        /// The regeneration rate, percent of the maximum a second.
        rate: f32,
        /// The rate multiplier, percent.
        rate_mult: f32,
    }

    extern "Rust" {
        /// A client's first activation of a target `distance` units away,
        /// `target_size` being its bounds' farthest point times its scale.
        fn activation_within_reach(distance: f32, target_size: f32) -> Verdict;
        /// Whether the race menu's result may set the race.
        fn race_allowed(facts: &RaceFacts) -> bool;
        /// Whether a player's melee hit on a player is within reach.
        fn melee_within_reach(facts: &MeleeFacts) -> Verdict;
        /// Whether a player's melee hit on a player is within the attacker's
        /// cone (the bound in degrees off its heading).
        fn melee_within_cone(facts: &ConeFacts) -> Verdict;
        /// The hit flags the server keeps.
        fn backed_flags(facts: &FlagFacts) -> Flags;
        /// TES3MP's rest switches from server-settings.json's `rest` block.
        type RestSettings;
        /// The switches from the block as JSON text (`{}` for both on); the
        /// error names the bad key.
        fn new_rest_settings(settings_json: &str) -> Result<Box<RestSettings>>;
        /// Whether a player's rest is let through, and why not.
        fn rest_check(settings: &RestSettings, facts: &RestFacts) -> RestRefusal;
        /// An attribute's percentage after a rest of `hours`.
        fn rest_after(regen: &Regen, hours: f32) -> f32;

        /// Every player actor's ground speed budget.
        type MovementBudgets;
        /// Empty budgets: each actor starts full.
        fn new_movement_budgets() -> Box<MovementBudgets>;
        /// Charge `actor` a move of `ground` units at `now_ms` (a monotonic
        /// clock); true when it fits.
        fn spend(self: &mut MovementBudgets, actor: u32, ground: f32, now_ms: u64) -> bool;
        /// Drop an actor's budget.
        fn forget(self: &mut MovementBudgets, actor: u32);

        /// The server's game clock and who has heard it.
        type GameClock;
        /// A clock from server-settings.json's `time` block as JSON text
        /// (`{}` for every default); the error names the bad key.
        fn new_game_clock(settings_json: &str) -> Result<Box<GameClock>>;
        /// The clock at `now_ms`, Unix milliseconds.
        fn now(self: &GameClock, now_ms: i64) -> GameTime;
        /// A player logs in and hears the clock.
        fn login(self: &mut GameClock, user: u32, now_ms: i64) -> GameTime;
        /// Whether a logged-in player is due to hear it again; a true
        /// answer counts as heard.
        fn resync_due(self: &mut GameClock, user: u32, now_ms: i64) -> bool;
        /// A player left.
        fn forget(self: &mut GameClock, user: u32);
    }
}

pub use ffi::{ConeFacts, FlagFacts, Flags, GameTime, MeleeFacts, RaceFacts, Regen, RestFacts, RestRefusal, Verdict};

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

fn melee_within_cone(f: &ConeFacts) -> Verdict {
    verdict(melee::within_cone(melee::ConeFacts {
        heading: f.heading,
        dx: f.dx,
        dy: f.dy,
        widest_strike_angle: f.widest_strike_angle,
        power: f.power,
        target_dead: f.target_dead,
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

/// TES3MP's rest switches (wire-rules rest).
pub struct RestSettings(rest::Switches);

fn new_rest_settings(settings_json: &str) -> Result<Box<RestSettings>, clock::SettingsError> {
    rest::Switches::from_json(settings_json).map(|s| Box::new(RestSettings(s)))
}

fn rest_check(settings: &RestSettings, f: &RestFacts) -> RestRefusal {
    let facts = rest::RestFacts {
        hours: f.hours,
        sleep: f.sleep,
        is_dead: f.is_dead,
        since_last_hit_ms: f.has_hit.then_some(f.since_last_hit_ms),
    };
    match rest::check(facts, settings.0) {
        Ok(()) => RestRefusal::Allowed,
        Err(rest::Refusal::Hours) => RestRefusal::Hours,
        Err(rest::Refusal::Off) => RestRefusal::Off,
        Err(rest::Refusal::Dead) => RestRefusal::Dead,
        Err(rest::Refusal::Fighting) => RestRefusal::Fighting,
    }
}

fn rest_after(r: &Regen, hours: f32) -> f32 {
    rest::after_rest(rest::Regen { percentage: r.percentage, rate: r.rate, rate_mult: r.rate_mult }, hours)
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

/// The server's game clock (wire-rules clock).
#[derive(Debug)]
pub struct GameClock(clock::Clock);

fn new_game_clock(settings_json: &str) -> Result<Box<GameClock>, clock::SettingsError> {
    clock::Clock::from_json(settings_json).map(|c| Box::new(GameClock(c)))
}

fn game_time(t: clock::GameTime) -> GameTime {
    GameTime {
        year: t.year,
        month: t.month,
        day: t.day,
        hour: t.hour,
        days_passed: t.days_passed,
        time_scale: t.time_scale,
    }
}

impl GameClock {
    fn now(&self, now_ms: i64) -> GameTime {
        game_time(self.0.now(now_ms))
    }

    fn login(&mut self, user: u32, now_ms: i64) -> GameTime {
        game_time(self.0.login(user, now_ms))
    }

    fn resync_due(&mut self, user: u32, now_ms: i64) -> bool {
        self.0.resync_due(user, now_ms)
    }

    fn forget(&mut self, user: u32) {
        self.0.forget(user);
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
        let behind = melee_within_cone(&ConeFacts {
            heading: 180.0,
            dx: 0.0,
            dy: 120.0,
            widest_strike_angle: 50.0,
            power: false,
            target_dead: false,
        });
        assert!(!behind.allowed && (behind.bound - 95.0).abs() < 1e-4);
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

    #[test]
    fn the_bridge_passes_the_rest_through() {
        let facts = |hours, is_dead, has_hit, since_last_hit_ms| RestFacts { hours, sleep: false, is_dead, has_hit, since_last_hit_ms };
        let on = new_rest_settings("{}");
        assert!(on.is_ok());
        let Ok(on) = on else { return };
        assert_eq!(rest_check(&on, &facts(8.0, false, false, 0)), RestRefusal::Allowed);
        assert_eq!(rest_check(&on, &facts(8.0, false, true, 10_000)), RestRefusal::Allowed);
        assert_eq!(rest_check(&on, &facts(8.0, false, true, 3_000)), RestRefusal::Fighting);
        assert_eq!(rest_check(&on, &facts(0.5, false, false, 0)), RestRefusal::Hours);
        assert_eq!(rest_check(&on, &facts(8.0, true, false, 0)), RestRefusal::Dead);
        let no_wait = new_rest_settings(r#"{"allowWait": false}"#);
        assert!(no_wait.is_ok());
        let Ok(no_wait) = no_wait else { return };
        assert_eq!(rest_check(&no_wait, &facts(8.0, false, false, 0)), RestRefusal::Off);
        assert!(new_rest_settings(r#"{"allowWait": "no"}"#).is_err());
        let half = Regen { percentage: 0.5, rate: 0.7, rate_mult: 1.0 };
        assert!((rest_after(&half, 1.0) - 0.5252).abs() < 1e-5);
    }

    #[test]
    fn the_bridge_passes_the_clock_through() {
        // 2026-10-03T00:00:00Z, the default epoch: Skyrim.esm's start
        let epoch = 1_790_985_600_000;
        assert!(new_game_clock("{}").is_ok());
        let Ok(mut c) = new_game_clock("{}") else { return };
        let t = c.login(3, epoch);
        assert_eq!((t.year, t.month, t.day, t.hour, t.time_scale), (201, 7, 17, 8.0, 20.0));
        assert!((t.days_passed - (1.0 + 8.0 / 24.0)).abs() < 1e-6);
        assert!(!c.resync_due(3, epoch + 1));
        assert!(c.resync_due(3, epoch + clock::RESYNC_MS));
        c.forget(3);
        assert!(new_game_clock(r#"{"timeScale": 0}"#).is_err());
    }
}
