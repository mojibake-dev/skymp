//! Damage flags (thuum docs/verbs/damage-flags.md). The client says whether
//! its hit was a power attack (twice the damage) and a sneak attack (1.3
//! times); a player keeps a flag only when the server saw it: a power
//! attack's start among its animation events within the last three seconds,
//! and its own sneaking state from its movement. A flag the server cannot
//! back is dropped and the hit lands as a plain one.
//!
//! A kept sneak attack is worth the game's base multiplier for the weapon's
//! type, from its settings (thuum docs/verbs/sneak-damage.md), not SkyMP's
//! flat 1.3.

/// How long after a power attack's start its hit may come.
pub const POWER_ATTACK_WINDOW_MS: u64 = 3_000;

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
}

/// The flags the server keeps.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Flags {
    /// Power attack.
    pub power: bool,
    /// Sneak attack.
    pub sneak: bool,
}

/// The flags a hit keeps.
pub fn backed_flags(f: FlagFacts) -> Flags {
    let saw_power = matches!(f.since_power_start_ms, Some(ms) if ms <= POWER_ATTACK_WINDOW_MS);
    Flags { power: f.claims_power && saw_power, sneak: f.claims_sneak && f.is_sneaking }
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
        _ => return SKYMP_SNEAK_MULT,
    };
    if setting.is_finite() && setting > 0.0 {
        setting
    } else {
        SKYMP_SNEAK_MULT
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn facts(power: bool, sneak: bool, since: Option<u64>, sneaking: bool) -> FlagFacts {
        FlagFacts { claims_power: power, claims_sneak: sneak, since_power_start_ms: since, is_sneaking: sneaking }
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
