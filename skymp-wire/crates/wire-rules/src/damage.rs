//! Damage flags (thuum docs/verbs/damage-flags.md). The client says whether
//! its hit was a power attack (twice the damage) and a sneak attack (1.3
//! times); a player keeps a flag only when the server saw it: a power
//! attack's start among its animation events within the last three seconds,
//! and its own sneaking state from its movement. A flag the server cannot
//! back is dropped and the hit lands as a plain one.
//!
//! A bash (thuum docs/verbs/blocking.md) is kept the same way: a bash's
//! start (bashStart) among the attacker's animation events within the last
//! three seconds; x-block-probe 20261011-021114 measured 951 ms from a
//! bashStart to its hit.
//!
//! A kept sneak attack is worth the game's base multiplier for the weapon's
//! type, from its settings (thuum docs/verbs/sneak-damage.md), not SkyMP's
//! flat 1.3.
//!
//! One difficulty, the server's, scales every hit an actor takes, as the
//! engine's DamageActor does on the victim's game: a player's damage by the
//! to-player table, every other actor's by the by-player table.

use crate::magic::EffectFacts;

/// How long after a power attack's start its hit may come.
pub const POWER_ATTACK_WINDOW_MS: u64 = 3_000;
/// How long after a bash's start its hit may come: the power attack's room
/// (951 ms measured, x-block-probe 20261011-021114).
pub const BASH_WINDOW_MS: u64 = 3_000;

/// What the server knows about a player's hit and its claims.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FlagFacts {
    /// The client says power attack.
    pub claims_power: bool,
    /// The client says sneak attack.
    pub claims_sneak: bool,
    /// Milliseconds since the attacker's last power attack start, if any.
    pub since_power_start_ms: Option<u64>,
    /// The server holds the attacker sneaking.
    pub is_sneaking: bool,
    /// The client says bash.
    pub claims_bash: bool,
    /// Milliseconds since the attacker's last bash start, if any.
    pub since_bash_start_ms: Option<u64>,
}

/// The flags the server keeps.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Flags {
    /// Power attack.
    pub power: bool,
    /// Sneak attack.
    pub sneak: bool,
    /// Bash.
    pub bash: bool,
}

/// The flags a hit keeps.
pub fn backed_flags(f: FlagFacts) -> Flags {
    let saw_power = matches!(f.since_power_start_ms, Some(ms) if ms <= POWER_ATTACK_WINDOW_MS);
    let saw_bash = matches!(f.since_bash_start_ms, Some(ms) if ms <= BASH_WINDOW_MS);
    Flags { power: f.claims_power && saw_power, sneak: f.claims_sneak && f.is_sneaking, bash: f.claims_bash && saw_bash }
}

/// SkyMP's flat sneak attack multiplier, kept for a weapon type the game names
/// no setting for: bows, crossbows, staffs (thuum docs/verbs/sneak-damage.md).
pub const SKYMP_SNEAK_MULT: f32 = 1.3;

/// The game's base sneak attack multipliers by weapon type, Skyrim.esm's
/// fCombatSneak*Mult settings, as the core read them from the master files; 0
/// or not a number for one it could not read.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct SneakMults {
    /// fCombatSneakHandMult.
    pub hand: f32,
    /// fCombatSneak1HSwordMult.
    pub one_hand_sword: f32,
    /// fCombatSneak1HDaggerMult.
    pub one_hand_dagger: f32,
    /// fCombatSneak1HAxeMult.
    pub one_hand_axe: f32,
    /// fCombatSneak1HMaceMult.
    pub one_hand_mace: f32,
    /// fCombatSneak2HSwordMult.
    pub two_hand_sword: f32,
    /// fCombatSneak2HAxeMult.
    pub two_hand_axe: f32,
    /// The ranged weapons' multiplier (bows, staffs, crossbows: DNAM animation
    /// types 7 to 9), which the game names no setting for (Skyrim.esm has no
    /// fCombatSneakBowMult, esm.py 2026-10-26): the server setting
    /// sneakRangedMult, measured against the engine by x-npc-sneak-shot
    /// (thuum docs/verbs/npc-fate.md, train 52); 0 keeps SkyMP's 1.3.
    pub ranged: f32,
}

/// A kept sneak attack's damage multiplier for a weapon whose WEAP record's
/// DNAM animation type is `anim_type` (0 hand to hand, 1 to 4 one-handed
/// sword, dagger, axe and mace, 5 and 6 two-handed sword and axe): the game's
/// setting for that type, else SkyMP's 1.3. Perks that raise it come with
/// progression (M5).
pub fn sneak_mult(anim_type: u8, m: SneakMults) -> f32 {
    let setting = match anim_type {
        0 => m.hand,
        1 => m.one_hand_sword,
        2 => m.one_hand_dagger,
        3 => m.one_hand_axe,
        4 => m.one_hand_mace,
        5 => m.two_hand_sword,
        6 => m.two_hand_axe,
        7..=9 => m.ranged,
        _ => return SKYMP_SNEAK_MULT,
    };
    if setting.is_finite() && setting > 0.0 {
        setting
    } else {
        SKYMP_SNEAK_MULT
    }
}

/// Adept, the game's default difficulty: both tables read 1.
pub const ADEPT: u8 = 2;
/// Legendary, the highest difficulty.
pub const LEGENDARY: u8 = 5;

/// The executable's multipliers on the damage the player takes, its
/// fDiffMultHPToPC{VE,E,N,H,VH,L} settings from Novice (0) to Legendary (5):
/// the Setting objects' values in SkyrimSE.exe 1.6.1170, the table 26503
/// reads when DamageActor's (37335) victim is the player.
pub const DIFF_MULT_HP_TO_PC: [f32; 6] = [0.5, 0.75, 1.0, 1.5, 2.0, 2.5];
/// The executable's multipliers on the damage any other actor takes, its
/// fDiffMultHPByPC{VE,E,N,H,VH,L} settings, the table 26503 reads for every
/// victim but the player.
pub const DIFF_MULT_HP_BY_PC: [f32; 6] = [2.0, 1.5, 1.0, 0.75, 0.5, 0.25];

/// The difficulty settings as the master files hold them, Novice to
/// Legendary; 0 or not a number for one no record sets, which keeps the
/// executable's (Update.esm sets fDiffMultHPToPCL to 3; no master sets the
/// other eleven).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct DifficultyMults {
    /// fDiffMultHPToPC*: the damage a player takes.
    pub to_pc: [f32; 6],
    /// fDiffMultHPByPC*: the damage any other actor takes.
    pub by_pc: [f32; 6],
}

/// The multiplier on the damage an actor takes at `difficulty` (0 Novice to
/// 5 Legendary): the to-player entry for a player, the by-player entry for
/// any other actor, as DamageActor (37335) applies it on the victim's game;
/// a master file's setting over the executable's. A difficulty past
/// Legendary takes none (1), where the engine would index past its tables.
pub fn difficulty_mult(difficulty: u8, victim_is_player: bool, files: &DifficultyMults) -> f32 {
    let (set, exe) = if victim_is_player {
        (&files.to_pc, &DIFF_MULT_HP_TO_PC)
    } else {
        (&files.by_pc, &DIFF_MULT_HP_BY_PC)
    };
    let i = usize::from(difficulty);
    match (set.get(i), exe.get(i)) {
        (Some(&v), _) if v.is_finite() && v > 0.0 => v,
        (_, Some(&v)) => v,
        _ => 1.0,
    }
}

/// Health's index in the engine's actor value list (CommonLibSSE-NG
/// include/RE/A/ActorValues.h:32, kHealth).
const HEALTH: u32 = 24;

/// Whether an effect's damage takes the difficulty: its primary value is
/// Health, its magnitude lowers it (Detrimental), and it is no Recover
/// effect. ValueModifierEffect::ModifyActorValue (35086) sends such a
/// Health loss through DamageActor (37335) with no flag, so the multiplier
/// applies; a Recover effect's change takes the modifier path without it.
pub fn effect_takes_difficulty(facts: &EffectFacts) -> bool {
    facts.av == HEALTH && facts.detrimental && !facts.recover
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::magic::EffectKind;

    #[test]
    fn legendary_triples_a_players_damage_and_quarters_everyone_elses() {
        let update = DifficultyMults { to_pc: [0.0, 0.0, 0.0, 0.0, 0.0, 3.0], ..DifficultyMults::default() };
        assert!((difficulty_mult(LEGENDARY, true, &update) - 3.0).abs() < f32::EPSILON);
        assert!((difficulty_mult(LEGENDARY, false, &update) - 0.25).abs() < f32::EPSILON);
        // the executable's own where no file sets one
        assert!((difficulty_mult(LEGENDARY, true, &DifficultyMults::default()) - 2.5).abs() < f32::EPSILON);
        assert!((difficulty_mult(0, true, &update) - 0.5).abs() < f32::EPSILON);
        assert!((difficulty_mult(0, false, &update) - 2.0).abs() < f32::EPSILON);
    }

    #[test]
    fn adept_changes_nothing_and_a_difficulty_past_legendary_takes_none() {
        let update = DifficultyMults { to_pc: [0.0, 0.0, 0.0, 0.0, 0.0, 3.0], ..DifficultyMults::default() };
        for victim_is_player in [true, false] {
            assert!((difficulty_mult(ADEPT, victim_is_player, &update) - 1.0).abs() < f32::EPSILON);
            assert!((difficulty_mult(6, victim_is_player, &update) - 1.0).abs() < f32::EPSILON);
            assert!((difficulty_mult(u8::MAX, victim_is_player, &update) - 1.0).abs() < f32::EPSILON);
        }
        let unreadable = DifficultyMults { to_pc: [f32::NAN, -1.0, 0.0, 0.0, 0.0, f32::INFINITY], ..DifficultyMults::default() };
        assert!((difficulty_mult(0, true, &unreadable) - 0.5).abs() < f32::EPSILON);
        assert!((difficulty_mult(1, true, &unreadable) - 0.75).abs() < f32::EPSILON);
        assert!((difficulty_mult(LEGENDARY, true, &unreadable) - 2.5).abs() < f32::EPSILON);
    }

    #[test]
    fn a_health_loss_takes_the_difficulty_and_a_heal_or_a_fortify_does_not() {
        let fire = EffectFacts {
            effect: 0x12f03,
            source: 0x12fd0,
            kind: EffectKind::Value,
            av: HEALTH,
            second_av: None,
            second_weight: 0.0,
            magnitude: 25.0,
            duration_s: 0.0,
            recover: false,
            detrimental: true,
            no_duration: false,
        };
        assert!(effect_takes_difficulty(&fire));
        assert!(!effect_takes_difficulty(&EffectFacts { detrimental: false, ..fire }));
        assert!(!effect_takes_difficulty(&EffectFacts { recover: true, ..fire }));
        assert!(!effect_takes_difficulty(&EffectFacts { av: HEALTH + 1, ..fire }));
    }

    #[test]
    fn ranged_weapons_take_the_measured_setting_or_skymps_number() {
        let m = SneakMults { ranged: 2.0, ..SneakMults::default() };
        assert_eq!(sneak_mult(7, m), 2.0);
        assert_eq!(sneak_mult(8, m), 2.0);
        assert_eq!(sneak_mult(9, m), 2.0);
        assert_eq!(sneak_mult(7, SneakMults::default()), SKYMP_SNEAK_MULT);
        assert_eq!(sneak_mult(7, SneakMults { ranged: f32::NAN, ..SneakMults::default() }), SKYMP_SNEAK_MULT);
    }

    const fn facts(power: bool, sneak: bool, since: Option<u64>, sneaking: bool) -> FlagFacts {
        FlagFacts {
            claims_power: power,
            claims_sneak: sneak,
            since_power_start_ms: since,
            is_sneaking: sneaking,
            claims_bash: false,
            since_bash_start_ms: None,
        }
    }

    #[test]
    fn a_bash_flag_needs_a_recent_bash_start() {
        let bash = |since: Option<u64>| backed_flags(FlagFacts { claims_bash: true, since_bash_start_ms: since, ..facts(false, false, None, false) });
        assert!(!bash(None).bash);
        assert!(bash(Some(951)).bash);
        assert!(!bash(Some(3_001)).bash);
        let unclaimed = FlagFacts { since_bash_start_ms: Some(10), ..facts(false, false, None, false) };
        assert!(!backed_flags(unclaimed).bash);
    }

    #[test]
    fn a_power_flag_needs_a_recent_power_attack_start() {
        assert!(!backed_flags(facts(true, false, None, false)).power);
        assert!(backed_flags(facts(true, false, Some(800), false)).power);
        assert!(!backed_flags(facts(true, false, Some(3_001), false)).power);
        assert!(!backed_flags(facts(false, false, Some(10), false)).power);
    }

    /// Skyrim.esm's settings (lab/esm.py find GMST CombatSneak, 2026-10-04).
    const SKYRIM: SneakMults = SneakMults {
        hand: 2.0,
        one_hand_sword: 3.0,
        one_hand_dagger: 3.0,
        one_hand_axe: 3.0,
        one_hand_mace: 3.0,
        two_hand_sword: 2.0,
        two_hand_axe: 2.0,
        ranged: 0.0,
    };

    #[test]
    fn the_game_s_multiplier_for_each_weapon_type() {
        let want = [(0u8, 2.0f32), (1, 3.0), (2, 3.0), (3, 3.0), (4, 3.0), (5, 2.0), (6, 2.0)];
        for (anim_type, mult) in want {
            assert!((sneak_mult(anim_type, SKYRIM) - mult).abs() < f32::EPSILON, "{anim_type}");
        }
        // bow, staff, crossbow and anything unknown keep SkyMP's 1.3
        for anim_type in [7u8, 8, 9, 10, 255] {
            assert!((sneak_mult(anim_type, SKYRIM) - SKYMP_SNEAK_MULT).abs() < f32::EPSILON, "{anim_type}");
        }
    }

    #[test]
    fn a_setting_the_core_could_not_read_keeps_1_3() {
        let missing = SneakMults { one_hand_sword: 0.0, hand: f32::NAN, two_hand_axe: -2.0, ..SKYRIM };
        for anim_type in [1u8, 0, 6] {
            assert!((sneak_mult(anim_type, missing) - SKYMP_SNEAK_MULT).abs() < f32::EPSILON, "{anim_type}");
        }
        assert!((sneak_mult(2, missing) - 3.0).abs() < f32::EPSILON);
    }

    #[test]
    fn a_sneak_flag_needs_sneaking() {
        assert!(!backed_flags(facts(false, true, None, false)).sneak);
        assert!(backed_flags(facts(false, true, None, true)).sneak);
        assert!(!backed_flags(facts(false, false, None, true)).sneak);
    }
}
